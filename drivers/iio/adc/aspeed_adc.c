// SPDX-License-Identifier: GPL-2.0-only
/*
 * Aspeed AST2400/2500/2600 ADC
 *
 * Copyright (C) 2017 Google, Inc.
 * Copyright (C) 2021 Aspeed Technology Inc.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/bitfield.h>

#include <linux/iio/iio.h>
#include <linux/iio/driver.h>
#include <linux/iopoll.h>

#define ASPEED_RESOLUTION_BITS		10
#define ASPEED_CLOCKS_PER_SAMPLE	12

#define ASPEED_REG_ENGINE_CONTROL	0x00
#define ASPEED_REG_INTERRUPT_CONTROL	0x04
#define ASPEED_REG_VGA_DETECT_CONTROL	0x08
#define ASPEED_REG_CLOCK_CONTROL	0x0C
#define ASPEED_REG_COMPENSATION_TRIM	0xC4
/*
 * The register offset between 0xC8~0xCC can be read and won't affect the
 * hardware logic in each version of ADC.
 */
#define ASPEED_REG_MAX			0xD0

#define ASPEED_ADC_ENGINE_ENABLE		BIT(0)
#define ASPEED_ADC_OP_MODE			GENMASK(3, 1)
#define ASPEED_ADC_OP_MODE_PWR_DOWN		0
#define ASPEED_ADC_OP_MODE_STANDBY		1
#define ASPEED_ADC_OP_MODE_NORMAL		7
#define ASPEED_ADC_CTRL_COMPENSATION		BIT(4)
#define ASPEED_ADC_AUTO_COMPENSATION		BIT(5)
/*
 * Bit 6 determines not only the reference voltage range but also the dividing
 * circuit for battery sensing.
 */
#define ASPEED_ADC_REF_VOLTAGE			GENMASK(7, 6)
#define ASPEED_ADC_REF_VOLTAGE_2500mV		0
#define ASPEED_ADC_REF_VOLTAGE_1200mV		1
#define ASPEED_ADC_REF_VOLTAGE_EXT_HIGH		2
#define ASPEED_ADC_REF_VOLTAGE_EXT_LOW		3
#define ASPEED_ADC_BAT_SENSING_DIV		BIT(6)
#define ASPEED_ADC_BAT_SENSING_DIV_2_3		0
#define ASPEED_ADC_BAT_SENSING_DIV_1_3		1
#define ASPEED_ADC_CTRL_INIT_RDY		BIT(8)
#define ASPEED_ADC_CH7_MODE			BIT(12)
#define ASPEED_ADC_CH7_NORMAL			0
#define ASPEED_ADC_CH7_BAT			1
#define ASPEED_ADC_BAT_SENSING_ENABLE		BIT(13)
#define ASPEED_ADC_CTRL_CHANNEL			GENMASK(31, 16)
#define ASPEED_ADC_CTRL_CHANNEL_ENABLE(ch)	FIELD_PREP(ASPEED_ADC_CTRL_CHANNEL, BIT(ch))

#define ASPEED_ADC_INIT_POLLING_TIME	500
#define ASPEED_ADC_INIT_TIMEOUT		500000

struct aspeed_adc_model_data {
	const char *model_name;
	unsigned int min_sampling_rate;	// Hz
	unsigned int max_sampling_rate;	// Hz
	unsigned int vref_fixed_mv;
	bool wait_init_sequence;
	bool need_prescaler;
	u8 scaler_bit_width;
	unsigned int num_channels;
};

struct aspeed_adc_data {
	struct device		*dev;
	const struct aspeed_adc_model_data *model_data;
	struct regulator	*regulator;
	void __iomem		*base;
	spinlock_t		clk_lock;
	struct clk_hw		*clk_prescaler;
	struct clk_hw		*clk_scaler;
	struct reset_control	*rst;
	int			vref_mv;
};

#define ASPEED_CHAN(_idx, _data_reg_addr) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = (_idx),					\
	.address = (_data_reg_addr),				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
				BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

static const struct iio_chan_spec aspeed_adc_iio_channels[] = {
	ASPEED_CHAN(0, 0x10),
	ASPEED_CHAN(1, 0x12),
	ASPEED_CHAN(2, 0x14),
	ASPEED_CHAN(3, 0x16),
	ASPEED_CHAN(4, 0x18),
	ASPEED_CHAN(5, 0x1A),
	ASPEED_CHAN(6, 0x1C),
	ASPEED_CHAN(7, 0x1E),
	ASPEED_CHAN(8, 0x20),
	ASPEED_CHAN(9, 0x22),
	ASPEED_CHAN(10, 0x24),
	ASPEED_CHAN(11, 0x26),
	ASPEED_CHAN(12, 0x28),
	ASPEED_CHAN(13, 0x2A),
	ASPEED_CHAN(14, 0x2C),
	ASPEED_CHAN(15, 0x2E),
};

static int aspeed_adc_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)
{
	struct aspeed_adc_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val = readw(data->base + chan->address);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = data->vref_mv;
		*val2 = ASPEED_RESOLUTION_BITS;
		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = clk_get_rate(data->clk_scaler->clk) /
				ASPEED_CLOCKS_PER_SAMPLE;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int aspeed_adc_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2, long mask)
{
	struct aspeed_adc_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (val < data->model_data->min_sampling_rate ||
			val > data->model_data->max_sampling_rate)
			return -EINVAL;

		clk_set_rate(data->clk_scaler->clk,
				val * ASPEED_CLOCKS_PER_SAMPLE);
		return 0;

	case IIO_CHAN_INFO_SCALE:
	case IIO_CHAN_INFO_RAW:
		/*
		 * Technically, these could be written but the only reasons
		 * for doing so seem better handled in userspace.  EPERM is
		 * returned to signal this is a policy choice rather than a
		 * hardware limitation.
		 */
		return -EPERM;

	default:
		return -EINVAL;
	}
}

static int aspeed_adc_reg_access(struct iio_dev *indio_dev,
				 unsigned int reg, unsigned int writeval,
				 unsigned int *readval)
{
	struct aspeed_adc_data *data = iio_priv(indio_dev);

	if (!readval || reg % 4 || reg > ASPEED_REG_MAX)
		return -EINVAL;

	*readval = readl(data->base + reg);

	return 0;
}

static const struct iio_info aspeed_adc_iio_info = {
	.read_raw = aspeed_adc_read_raw,
	.write_raw = aspeed_adc_write_raw,
	.debugfs_reg_access = aspeed_adc_reg_access,
};

static void aspeed_adc_reset_assert(void *data)
{
	struct reset_control *rst = data;

	reset_control_assert(rst);
}

static void aspeed_adc_clk_disable_unprepare(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
}

static void aspeed_adc_power_down(void *data)
{
	struct aspeed_adc_data *priv_data = data;

	writel(FIELD_PREP(ASPEED_ADC_OP_MODE, ASPEED_ADC_OP_MODE_PWR_DOWN),
	       priv_data->base + ASPEED_REG_ENGINE_CONTROL);
}

static void aspeed_adc_reg_disable(void *data)
{
	struct regulator *reg = data;

	regulator_disable(reg);
}

static int aspeed_adc_vref_config(struct iio_dev *indio_dev)
{
	struct aspeed_adc_data *data = iio_priv(indio_dev);
	int ret;
	u32 adc_engine_control_reg_val;

	if (data->model_data->vref_fixed_mv) {
		data->vref_mv = data->model_data->vref_fixed_mv;
		return 0;
	}
	adc_engine_control_reg_val =
		readl(data->base + ASPEED_REG_ENGINE_CONTROL);
	data->regulator = devm_regulator_get_optional(data->dev, "vref");
	if (!IS_ERR(data->regulator)) {
		ret = regulator_enable(data->regulator);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(
			data->dev, aspeed_adc_reg_disable, data->regulator);
		if (ret)
			return ret;
		data->vref_mv = regulator_get_voltage(data->regulator);
		/* Conversion from uV to mV */
		data->vref_mv /= 1000;
		if ((data->vref_mv >= 1550) && (data->vref_mv <= 2700))
			writel(adc_engine_control_reg_val |
				FIELD_PREP(
					ASPEED_ADC_REF_VOLTAGE,
					ASPEED_ADC_REF_VOLTAGE_EXT_HIGH),
			data->base + ASPEED_REG_ENGINE_CONTROL);
		else if ((data->vref_mv >= 900) && (data->vref_mv <= 1650))
			writel(adc_engine_control_reg_val |
				FIELD_PREP(
					ASPEED_ADC_REF_VOLTAGE,
					ASPEED_ADC_REF_VOLTAGE_EXT_LOW),
			data->base + ASPEED_REG_ENGINE_CONTROL);
		else {
			dev_err(data->dev, "Regulator voltage %d not support",
				data->vref_mv);
			return -EOPNOTSUPP;
		}
	} else {
		if (PTR_ERR(data->regulator) != -ENODEV)
			return PTR_ERR(data->regulator);
		data->vref_mv = 2500000;
		of_property_read_u32(data->dev->of_node,
				     "aspeed,int-vref-microvolt",
				     &data->vref_mv);
		/* Conversion from uV to mV */
		data->vref_mv /= 1000;
		if (data->vref_mv == 2500)
			writel(adc_engine_control_reg_val |
				FIELD_PREP(ASPEED_ADC_REF_VOLTAGE,
						ASPEED_ADC_REF_VOLTAGE_2500mV),
			data->base + ASPEED_REG_ENGINE_CONTROL);
		else if (data->vref_mv == 1200)
			writel(adc_engine_control_reg_val |
				FIELD_PREP(ASPEED_ADC_REF_VOLTAGE,
						ASPEED_ADC_REF_VOLTAGE_1200mV),
			data->base + ASPEED_REG_ENGINE_CONTROL);
		else {
			dev_err(data->dev, "Voltage %d not support", data->vref_mv);
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static int aspeed_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct aspeed_adc_data *data;
	int ret;
	u32 adc_engine_control_reg_val;
	unsigned long scaler_flags = 0;
	char clk_name[32], clk_parent_name[32];

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = &pdev->dev;
	data->model_data = of_device_get_match_data(&pdev->dev);

	data->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	/* Register ADC clock prescaler with source specified by device tree. */
	spin_lock_init(&data->clk_lock);
	snprintf(clk_parent_name, ARRAY_SIZE(clk_parent_name), "%s",
		 of_clk_get_parent_name(pdev->dev.of_node, 0));

	if (data->model_data->need_prescaler) {
		snprintf(clk_name, ARRAY_SIZE(clk_name), "%s-prescaler",
			 data->model_data->model_name);
		data->clk_prescaler = devm_clk_hw_register_divider(
			&pdev->dev, clk_name, clk_parent_name, 0,
			data->base + ASPEED_REG_CLOCK_CONTROL, 17, 15, 0,
			&data->clk_lock);
		if (IS_ERR(data->clk_prescaler))
			return PTR_ERR(data->clk_prescaler);
		snprintf(clk_parent_name, ARRAY_SIZE(clk_parent_name),
			 clk_name);
		scaler_flags = CLK_SET_RATE_PARENT;
	}
	/*
	 * Register ADC clock scaler downstream from the prescaler. Allow rate
	 * setting to adjust the prescaler as well.
	 */
	snprintf(clk_name, ARRAY_SIZE(clk_name), "%s-scaler",
		 data->model_data->model_name);
	data->clk_scaler = devm_clk_hw_register_divider(
		&pdev->dev, clk_name, clk_parent_name, scaler_flags,
		data->base + ASPEED_REG_CLOCK_CONTROL, 0,
		data->model_data->scaler_bit_width, 0, &data->clk_lock);
	if (IS_ERR(data->clk_scaler))
		return PTR_ERR(data->clk_scaler);

	data->rst = devm_reset_control_get_shared(&pdev->dev, NULL);
	if (IS_ERR(data->rst)) {
		dev_err(&pdev->dev,
			"invalid or missing reset controller device tree entry");
		return PTR_ERR(data->rst);
	}
	reset_control_deassert(data->rst);

	ret = devm_add_action_or_reset(data->dev, aspeed_adc_reset_assert,
				       data->rst);
	if (ret)
		return ret;

	ret = aspeed_adc_vref_config(indio_dev);
	if (ret)
		return ret;

	adc_engine_control_reg_val =
		readl(data->base + ASPEED_REG_ENGINE_CONTROL);
	adc_engine_control_reg_val |=
		FIELD_PREP(ASPEED_ADC_OP_MODE, ASPEED_ADC_OP_MODE_NORMAL) |
		ASPEED_ADC_ENGINE_ENABLE;
	/* Enable engine in normal mode. */
	writel(adc_engine_control_reg_val,
	       data->base + ASPEED_REG_ENGINE_CONTROL);

	ret = devm_add_action_or_reset(data->dev, aspeed_adc_power_down,
					data);
	if (ret)
		return ret;

	if (data->model_data->wait_init_sequence) {
		/* Wait for initial sequence complete. */
		ret = readl_poll_timeout(data->base + ASPEED_REG_ENGINE_CONTROL,
					 adc_engine_control_reg_val,
					 adc_engine_control_reg_val &
					 ASPEED_ADC_CTRL_INIT_RDY,
					 ASPEED_ADC_INIT_POLLING_TIME,
					 ASPEED_ADC_INIT_TIMEOUT);
		if (ret)
			return ret;
	}

	ret = clk_prepare_enable(data->clk_scaler->clk);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(data->dev,
				       aspeed_adc_clk_disable_unprepare,
				       data->clk_scaler->clk);
	if (ret)
		return ret;

	/* Start all channels in normal mode. */
	adc_engine_control_reg_val =
		readl(data->base + ASPEED_REG_ENGINE_CONTROL);
	adc_engine_control_reg_val |= ASPEED_ADC_CTRL_CHANNEL;
	writel(adc_engine_control_reg_val,
	       data->base + ASPEED_REG_ENGINE_CONTROL);

	indio_dev->name = data->model_data->model_name;
	indio_dev->info = &aspeed_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = aspeed_adc_iio_channels;
	indio_dev->num_channels = data->model_data->num_channels;

	ret = devm_iio_device_register(data->dev, indio_dev);
	return ret;
}

static const struct aspeed_adc_model_data ast2400_model_data = {
	.model_name = "ast2400-adc",
	.vref_fixed_mv = 2500,
	.min_sampling_rate = 10000,
	.max_sampling_rate = 500000,
	.need_prescaler = true,
	.scaler_bit_width = 10,
	.num_channels = 16,
};

static const struct aspeed_adc_model_data ast2500_model_data = {
	.model_name = "ast2500-adc",
	.vref_fixed_mv = 1800,
	.min_sampling_rate = 1,
	.max_sampling_rate = 1000000,
	.wait_init_sequence = true,
	.need_prescaler = true,
	.scaler_bit_width = 10,
	.num_channels = 16,
};

static const struct aspeed_adc_model_data ast2600_adc0_model_data = {
	.model_name = "ast2600-adc0",
	.min_sampling_rate = 10000,
	.max_sampling_rate = 500000,
	.wait_init_sequence = true,
	.scaler_bit_width = 16,
	.num_channels = 8,
};

static const struct aspeed_adc_model_data ast2600_adc1_model_data = {
	.model_name = "ast2600-adc1",
	.min_sampling_rate = 10000,
	.max_sampling_rate = 500000,
	.wait_init_sequence = true,
	.scaler_bit_width = 16,
	.num_channels = 8,
};

static const struct of_device_id aspeed_adc_matches[] = {
	{ .compatible = "aspeed,ast2400-adc", .data = &ast2400_model_data },
	{ .compatible = "aspeed,ast2500-adc", .data = &ast2500_model_data },
	{ .compatible = "aspeed,ast2600-adc0", .data = &ast2600_adc0_model_data },
	{ .compatible = "aspeed,ast2600-adc1", .data = &ast2600_adc1_model_data },
	{},
};
MODULE_DEVICE_TABLE(of, aspeed_adc_matches);

static struct platform_driver aspeed_adc_driver = {
	.probe = aspeed_adc_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = aspeed_adc_matches,
	}
};

module_platform_driver(aspeed_adc_driver);

MODULE_AUTHOR("Rick Altherr <raltherr@google.com>");
MODULE_DESCRIPTION("Aspeed AST2400/2500/2600 ADC Driver");
MODULE_LICENSE("GPL");
