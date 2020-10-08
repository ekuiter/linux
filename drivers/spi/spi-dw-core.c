// SPDX-License-Identifier: GPL-2.0-only
/*
 * Designware SPI core controller driver (refer pxa2xx_spi.c)
 *
 * Copyright (c) 2009, Intel Corporation.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/of.h>

#include "spi-dw.h"

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#endif

/* Slave spi_device related */
struct chip_data {
	u32 cr0;
	u32 rx_sample_dly;	/* RX sample delay */
};

#ifdef CONFIG_DEBUG_FS

#define DW_SPI_DBGFS_REG(_name, _off)	\
{					\
	.name = _name,			\
	.offset = _off,			\
}

static const struct debugfs_reg32 dw_spi_dbgfs_regs[] = {
	DW_SPI_DBGFS_REG("CTRLR0", DW_SPI_CTRLR0),
	DW_SPI_DBGFS_REG("CTRLR1", DW_SPI_CTRLR1),
	DW_SPI_DBGFS_REG("SSIENR", DW_SPI_SSIENR),
	DW_SPI_DBGFS_REG("SER", DW_SPI_SER),
	DW_SPI_DBGFS_REG("BAUDR", DW_SPI_BAUDR),
	DW_SPI_DBGFS_REG("TXFTLR", DW_SPI_TXFTLR),
	DW_SPI_DBGFS_REG("RXFTLR", DW_SPI_RXFTLR),
	DW_SPI_DBGFS_REG("TXFLR", DW_SPI_TXFLR),
	DW_SPI_DBGFS_REG("RXFLR", DW_SPI_RXFLR),
	DW_SPI_DBGFS_REG("SR", DW_SPI_SR),
	DW_SPI_DBGFS_REG("IMR", DW_SPI_IMR),
	DW_SPI_DBGFS_REG("ISR", DW_SPI_ISR),
	DW_SPI_DBGFS_REG("DMACR", DW_SPI_DMACR),
	DW_SPI_DBGFS_REG("DMATDLR", DW_SPI_DMATDLR),
	DW_SPI_DBGFS_REG("DMARDLR", DW_SPI_DMARDLR),
	DW_SPI_DBGFS_REG("RX_SAMPLE_DLY", DW_SPI_RX_SAMPLE_DLY),
};

static int dw_spi_debugfs_init(struct dw_spi *dws)
{
	char name[32];

	snprintf(name, 32, "dw_spi%d", dws->master->bus_num);
	dws->debugfs = debugfs_create_dir(name, NULL);
	if (!dws->debugfs)
		return -ENOMEM;

	dws->regset.regs = dw_spi_dbgfs_regs;
	dws->regset.nregs = ARRAY_SIZE(dw_spi_dbgfs_regs);
	dws->regset.base = dws->regs;
	debugfs_create_regset32("registers", 0400, dws->debugfs, &dws->regset);

	return 0;
}

static void dw_spi_debugfs_remove(struct dw_spi *dws)
{
	debugfs_remove_recursive(dws->debugfs);
}

#else
static inline int dw_spi_debugfs_init(struct dw_spi *dws)
{
	return 0;
}

static inline void dw_spi_debugfs_remove(struct dw_spi *dws)
{
}
#endif /* CONFIG_DEBUG_FS */

void dw_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	bool cs_high = !!(spi->mode & SPI_CS_HIGH);

	/*
	 * DW SPI controller demands any native CS being set in order to
	 * proceed with data transfer. So in order to activate the SPI
	 * communications we must set a corresponding bit in the Slave
	 * Enable register no matter whether the SPI core is configured to
	 * support active-high or active-low CS level.
	 */
	if (cs_high == enable)
		dw_writel(dws, DW_SPI_SER, BIT(spi->chip_select));
	else
		dw_writel(dws, DW_SPI_SER, 0);
}
EXPORT_SYMBOL_GPL(dw_spi_set_cs);

/* Return the max entries we can fill into tx fifo */
static inline u32 tx_max(struct dw_spi *dws)
{
	u32 tx_room, rxtx_gap;

	tx_room = dws->fifo_len - dw_readl(dws, DW_SPI_TXFLR);

	/*
	 * Another concern is about the tx/rx mismatch, we
	 * though to use (dws->fifo_len - rxflr - txflr) as
	 * one maximum value for tx, but it doesn't cover the
	 * data which is out of tx/rx fifo and inside the
	 * shift registers. So a control from sw point of
	 * view is taken.
	 */
	rxtx_gap = dws->fifo_len - (dws->rx_len - dws->tx_len);

	return min3((u32)dws->tx_len, tx_room, rxtx_gap);
}

/* Return the max entries we should read out of rx fifo */
static inline u32 rx_max(struct dw_spi *dws)
{
	return min_t(u32, dws->rx_len, dw_readl(dws, DW_SPI_RXFLR));
}

static void dw_writer(struct dw_spi *dws)
{
	u32 max = tx_max(dws);
	u16 txw = 0;

	while (max--) {
		if (dws->tx) {
			if (dws->n_bytes == 1)
				txw = *(u8 *)(dws->tx);
			else
				txw = *(u16 *)(dws->tx);

			dws->tx += dws->n_bytes;
		}
		dw_write_io_reg(dws, DW_SPI_DR, txw);
		--dws->tx_len;
	}
}

static void dw_reader(struct dw_spi *dws)
{
	u32 max = rx_max(dws);
	u16 rxw;

	while (max--) {
		rxw = dw_read_io_reg(dws, DW_SPI_DR);
		if (dws->rx) {
			if (dws->n_bytes == 1)
				*(u8 *)(dws->rx) = rxw;
			else
				*(u16 *)(dws->rx) = rxw;

			dws->rx += dws->n_bytes;
		}
		--dws->rx_len;
	}
}

static void int_error_stop(struct dw_spi *dws, const char *msg)
{
	spi_reset_chip(dws);

	dev_err(&dws->master->dev, "%s\n", msg);
	dws->master->cur_msg->status = -EIO;
	spi_finalize_current_transfer(dws->master);
}

static irqreturn_t dw_spi_transfer_handler(struct dw_spi *dws)
{
	u16 irq_status = dw_readl(dws, DW_SPI_ISR);

	/* Error handling */
	if (irq_status & (SPI_INT_TXOI | SPI_INT_RXOI | SPI_INT_RXUI)) {
		dw_readl(dws, DW_SPI_ICR);
		int_error_stop(dws, "interrupt_transfer: fifo overrun/underrun");
		return IRQ_HANDLED;
	}

	/*
	 * Read data from the Rx FIFO every time we've got a chance executing
	 * this method. If there is nothing left to receive, terminate the
	 * procedure. Otherwise adjust the Rx FIFO Threshold level if it's a
	 * final stage of the transfer. By doing so we'll get the next IRQ
	 * right when the leftover incoming data is received.
	 */
	dw_reader(dws);
	if (!dws->rx_len) {
		spi_mask_intr(dws, 0xff);
		spi_finalize_current_transfer(dws->master);
	} else if (dws->rx_len <= dw_readl(dws, DW_SPI_RXFTLR)) {
		dw_writel(dws, DW_SPI_RXFTLR, dws->rx_len - 1);
	}

	/*
	 * Send data out if Tx FIFO Empty IRQ is received. The IRQ will be
	 * disabled after the data transmission is finished so not to
	 * have the TXE IRQ flood at the final stage of the transfer.
	 */
	if (irq_status & SPI_INT_TXEI) {
		dw_writer(dws);
		if (!dws->tx_len)
			spi_mask_intr(dws, SPI_INT_TXEI);
	}

	return IRQ_HANDLED;
}

static irqreturn_t dw_spi_irq(int irq, void *dev_id)
{
	struct spi_controller *master = dev_id;
	struct dw_spi *dws = spi_controller_get_devdata(master);
	u16 irq_status = dw_readl(dws, DW_SPI_ISR) & 0x3f;

	if (!irq_status)
		return IRQ_NONE;

	if (!master->cur_msg) {
		spi_mask_intr(dws, 0xff);
		return IRQ_HANDLED;
	}

	return dws->transfer_handler(dws);
}

static u32 dw_spi_prepare_cr0(struct dw_spi *dws, struct spi_device *spi)
{
	u32 cr0 = 0;

	if (!(dws->caps & DW_SPI_CAP_DWC_SSI)) {
		/* CTRLR0[ 5: 4] Frame Format */
		cr0 |= SSI_MOTO_SPI << SPI_FRF_OFFSET;

		/*
		 * SPI mode (SCPOL|SCPH)
		 * CTRLR0[ 6] Serial Clock Phase
		 * CTRLR0[ 7] Serial Clock Polarity
		 */
		cr0 |= ((spi->mode & SPI_CPOL) ? 1 : 0) << SPI_SCOL_OFFSET;
		cr0 |= ((spi->mode & SPI_CPHA) ? 1 : 0) << SPI_SCPH_OFFSET;

		/* CTRLR0[11] Shift Register Loop */
		cr0 |= ((spi->mode & SPI_LOOP) ? 1 : 0) << SPI_SRL_OFFSET;
	} else {
		/* CTRLR0[ 7: 6] Frame Format */
		cr0 |= SSI_MOTO_SPI << DWC_SSI_CTRLR0_FRF_OFFSET;

		/*
		 * SPI mode (SCPOL|SCPH)
		 * CTRLR0[ 8] Serial Clock Phase
		 * CTRLR0[ 9] Serial Clock Polarity
		 */
		cr0 |= ((spi->mode & SPI_CPOL) ? 1 : 0) << DWC_SSI_CTRLR0_SCPOL_OFFSET;
		cr0 |= ((spi->mode & SPI_CPHA) ? 1 : 0) << DWC_SSI_CTRLR0_SCPH_OFFSET;

		/* CTRLR0[13] Shift Register Loop */
		cr0 |= ((spi->mode & SPI_LOOP) ? 1 : 0) << DWC_SSI_CTRLR0_SRL_OFFSET;

		if (dws->caps & DW_SPI_CAP_KEEMBAY_MST)
			cr0 |= DWC_SSI_CTRLR0_KEEMBAY_MST;
	}

	return cr0;
}

void dw_spi_update_config(struct dw_spi *dws, struct spi_device *spi,
			  struct dw_spi_cfg *cfg)
{
	struct chip_data *chip = spi_get_ctldata(spi);
	u32 cr0 = chip->cr0;
	u32 speed_hz;
	u16 clk_div;

	/* CTRLR0[ 4/3: 0] Data Frame Size */
	cr0 |= (cfg->dfs - 1);

	if (!(dws->caps & DW_SPI_CAP_DWC_SSI))
		/* CTRLR0[ 9:8] Transfer Mode */
		cr0 |= cfg->tmode << SPI_TMOD_OFFSET;
	else
		/* CTRLR0[11:10] Transfer Mode */
		cr0 |= cfg->tmode << DWC_SSI_CTRLR0_TMOD_OFFSET;

	dw_writel(dws, DW_SPI_CTRLR0, cr0);

	if (cfg->tmode == SPI_TMOD_EPROMREAD || cfg->tmode == SPI_TMOD_RO)
		dw_writel(dws, DW_SPI_CTRLR1, cfg->ndf ? cfg->ndf - 1 : 0);

	/* Note DW APB SSI clock divider doesn't support odd numbers */
	clk_div = (DIV_ROUND_UP(dws->max_freq, cfg->freq) + 1) & 0xfffe;
	speed_hz = dws->max_freq / clk_div;

	if (dws->current_freq != speed_hz) {
		spi_set_clk(dws, clk_div);
		dws->current_freq = speed_hz;
	}

	/* Update RX sample delay if required */
	if (dws->cur_rx_sample_dly != chip->rx_sample_dly) {
		dw_writel(dws, DW_SPI_RX_SAMPLE_DLY, chip->rx_sample_dly);
		dws->cur_rx_sample_dly = chip->rx_sample_dly;
	}
}
EXPORT_SYMBOL_GPL(dw_spi_update_config);

static void dw_spi_irq_setup(struct dw_spi *dws)
{
	u16 level;
	u8 imask;

	/*
	 * Originally Tx and Rx data lengths match. Rx FIFO Threshold level
	 * will be adjusted at the final stage of the IRQ-based SPI transfer
	 * execution so not to lose the leftover of the incoming data.
	 */
	level = min_t(u16, dws->fifo_len / 2, dws->tx_len);
	dw_writel(dws, DW_SPI_TXFTLR, level);
	dw_writel(dws, DW_SPI_RXFTLR, level - 1);

	imask = SPI_INT_TXEI | SPI_INT_TXOI | SPI_INT_RXUI | SPI_INT_RXOI |
		SPI_INT_RXFI;
	spi_umask_intr(dws, imask);

	dws->transfer_handler = dw_spi_transfer_handler;
}

static int dw_spi_transfer_one(struct spi_controller *master,
		struct spi_device *spi, struct spi_transfer *transfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);
	struct dw_spi_cfg cfg = {
		.tmode = SPI_TMOD_TR,
		.dfs = transfer->bits_per_word,
		.freq = transfer->speed_hz,
	};
	int ret;

	dws->dma_mapped = 0;
	dws->n_bytes = DIV_ROUND_UP(transfer->bits_per_word, BITS_PER_BYTE);
	dws->tx = (void *)transfer->tx_buf;
	dws->tx_len = transfer->len / dws->n_bytes;
	dws->rx = transfer->rx_buf;
	dws->rx_len = dws->tx_len;

	/* Ensure the data above is visible for all CPUs */
	smp_mb();

	spi_enable_chip(dws, 0);

	dw_spi_update_config(dws, spi, &cfg);

	transfer->effective_speed_hz = dws->current_freq;

	/* Check if current transfer is a DMA transaction */
	if (master->can_dma && master->can_dma(master, spi, transfer))
		dws->dma_mapped = master->cur_msg_mapped;

	/* For poll mode just disable all interrupts */
	spi_mask_intr(dws, 0xff);

	if (dws->dma_mapped) {
		ret = dws->dma_ops->dma_setup(dws, transfer);
		if (ret)
			return ret;
	}

	spi_enable_chip(dws, 1);

	if (dws->dma_mapped)
		return dws->dma_ops->dma_transfer(dws, transfer);

	dw_spi_irq_setup(dws);

	return 1;
}

static void dw_spi_handle_err(struct spi_controller *master,
		struct spi_message *msg)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);

	if (dws->dma_mapped)
		dws->dma_ops->dma_stop(dws);

	spi_reset_chip(dws);
}

/* This may be called twice for each spi dev */
static int dw_spi_setup(struct spi_device *spi)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct chip_data *chip;

	/* Only alloc on first setup */
	chip = spi_get_ctldata(spi);
	if (!chip) {
		struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
		u32 rx_sample_dly_ns;

		chip = kzalloc(sizeof(struct chip_data), GFP_KERNEL);
		if (!chip)
			return -ENOMEM;
		spi_set_ctldata(spi, chip);
		/* Get specific / default rx-sample-delay */
		if (device_property_read_u32(&spi->dev,
					     "rx-sample-delay-ns",
					     &rx_sample_dly_ns) != 0)
			/* Use default controller value */
			rx_sample_dly_ns = dws->def_rx_sample_dly_ns;
		chip->rx_sample_dly = DIV_ROUND_CLOSEST(rx_sample_dly_ns,
							NSEC_PER_SEC /
							dws->max_freq);
	}

	/*
	 * Update CR0 data each time the setup callback is invoked since
	 * the device parameters could have been changed, for instance, by
	 * the MMC SPI driver or something else.
	 */
	chip->cr0 = dw_spi_prepare_cr0(dws, spi);

	return 0;
}

static void dw_spi_cleanup(struct spi_device *spi)
{
	struct chip_data *chip = spi_get_ctldata(spi);

	kfree(chip);
	spi_set_ctldata(spi, NULL);
}

/* Restart the controller, disable all interrupts, clean rx fifo */
static void spi_hw_init(struct device *dev, struct dw_spi *dws)
{
	spi_reset_chip(dws);

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec
	 */
	if (!dws->fifo_len) {
		u32 fifo;

		for (fifo = 1; fifo < 256; fifo++) {
			dw_writel(dws, DW_SPI_TXFTLR, fifo);
			if (fifo != dw_readl(dws, DW_SPI_TXFTLR))
				break;
		}
		dw_writel(dws, DW_SPI_TXFTLR, 0);

		dws->fifo_len = (fifo == 1) ? 0 : fifo;
		dev_dbg(dev, "Detected FIFO size: %u bytes\n", dws->fifo_len);
	}

	/* enable HW fixup for explicit CS deselect for Amazon's alpine chip */
	if (dws->caps & DW_SPI_CAP_CS_OVERRIDE)
		dw_writel(dws, DW_SPI_CS_OVERRIDE, 0xF);
}

int dw_spi_add_host(struct device *dev, struct dw_spi *dws)
{
	struct spi_controller *master;
	int ret;

	if (!dws)
		return -EINVAL;

	master = spi_alloc_master(dev, 0);
	if (!master)
		return -ENOMEM;

	dws->master = master;
	dws->dma_addr = (dma_addr_t)(dws->paddr + DW_SPI_DR);

	spi_controller_set_devdata(master, dws);

	/* Basic HW init */
	spi_hw_init(dev, dws);

	ret = request_irq(dws->irq, dw_spi_irq, IRQF_SHARED, dev_name(dev),
			  master);
	if (ret < 0) {
		dev_err(dev, "can not get IRQ\n");
		goto err_free_master;
	}

	master->use_gpio_descriptors = true;
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LOOP;
	master->bits_per_word_mask =  SPI_BPW_RANGE_MASK(4, 16);
	master->bus_num = dws->bus_num;
	master->num_chipselect = dws->num_cs;
	master->setup = dw_spi_setup;
	master->cleanup = dw_spi_cleanup;
	if (dws->set_cs)
		master->set_cs = dws->set_cs;
	else
		master->set_cs = dw_spi_set_cs;
	master->transfer_one = dw_spi_transfer_one;
	master->handle_err = dw_spi_handle_err;
	master->max_speed_hz = dws->max_freq;
	master->dev.of_node = dev->of_node;
	master->dev.fwnode = dev->fwnode;
	master->flags = SPI_MASTER_GPIO_SS;
	master->auto_runtime_pm = true;

	/* Get default rx sample delay */
	device_property_read_u32(dev, "rx-sample-delay-ns",
				 &dws->def_rx_sample_dly_ns);

	if (dws->dma_ops && dws->dma_ops->dma_init) {
		ret = dws->dma_ops->dma_init(dev, dws);
		if (ret) {
			dev_warn(dev, "DMA init failed\n");
		} else {
			master->can_dma = dws->dma_ops->can_dma;
			master->flags |= SPI_CONTROLLER_MUST_TX;
		}
	}

	ret = spi_register_controller(master);
	if (ret) {
		dev_err(&master->dev, "problem registering spi master\n");
		goto err_dma_exit;
	}

	dw_spi_debugfs_init(dws);
	return 0;

err_dma_exit:
	if (dws->dma_ops && dws->dma_ops->dma_exit)
		dws->dma_ops->dma_exit(dws);
	spi_enable_chip(dws, 0);
	free_irq(dws->irq, master);
err_free_master:
	spi_controller_put(master);
	return ret;
}
EXPORT_SYMBOL_GPL(dw_spi_add_host);

void dw_spi_remove_host(struct dw_spi *dws)
{
	dw_spi_debugfs_remove(dws);

	spi_unregister_controller(dws->master);

	if (dws->dma_ops && dws->dma_ops->dma_exit)
		dws->dma_ops->dma_exit(dws);

	spi_shutdown_chip(dws);

	free_irq(dws->irq, dws->master);
}
EXPORT_SYMBOL_GPL(dw_spi_remove_host);

int dw_spi_suspend_host(struct dw_spi *dws)
{
	int ret;

	ret = spi_controller_suspend(dws->master);
	if (ret)
		return ret;

	spi_shutdown_chip(dws);
	return 0;
}
EXPORT_SYMBOL_GPL(dw_spi_suspend_host);

int dw_spi_resume_host(struct dw_spi *dws)
{
	spi_hw_init(&dws->master->dev, dws);
	return spi_controller_resume(dws->master);
}
EXPORT_SYMBOL_GPL(dw_spi_resume_host);

MODULE_AUTHOR("Feng Tang <feng.tang@intel.com>");
MODULE_DESCRIPTION("Driver for DesignWare SPI controller core");
MODULE_LICENSE("GPL v2");
