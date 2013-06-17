/*
 * addi_apci_3xxx.c
 * Copyright (C) 2004,2005  ADDI-DATA GmbH for the source code of this module.
 * Project manager: S. Weber
 *
 *	ADDI-DATA GmbH
 *	Dieselstrasse 3
 *	D-77833 Ottersweier
 *	Tel: +19(0)7223/9493-0
 *	Fax: +49(0)7223/9493-92
 *	http://www.addi-data.com
 *	info@addi-data.com
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/pci.h>
#include <linux/interrupt.h>

#include "../comedidev.h"

#define CONV_UNIT_NS		(1 << 0)
#define CONV_UNIT_US		(1 << 1)
#define CONV_UNIT_MS		(1 << 2)

static const struct comedi_lrange apci3xxx_ai_range = {
	8, {
		BIP_RANGE(10),
		BIP_RANGE(5),
		BIP_RANGE(2),
		BIP_RANGE(1),
		UNI_RANGE(10),
		UNI_RANGE(5),
		UNI_RANGE(2),
		UNI_RANGE(1)
	}
};

static const struct comedi_lrange apci3xxx_ao_range = {
	2, {
		BIP_RANGE(10),
		UNI_RANGE(10)
	}
};

enum apci3xxx_boardid {
	BOARD_APCI3000_16,
	BOARD_APCI3000_8,
	BOARD_APCI3000_4,
	BOARD_APCI3006_16,
	BOARD_APCI3006_8,
	BOARD_APCI3006_4,
	BOARD_APCI3010_16,
	BOARD_APCI3010_8,
	BOARD_APCI3010_4,
	BOARD_APCI3016_16,
	BOARD_APCI3016_8,
	BOARD_APCI3016_4,
	BOARD_APCI3100_16_4,
	BOARD_APCI3100_8_4,
	BOARD_APCI3106_16_4,
	BOARD_APCI3106_8_4,
	BOARD_APCI3110_16_4,
	BOARD_APCI3110_8_4,
	BOARD_APCI3116_16_4,
	BOARD_APCI3116_8_4,
	BOARD_APCI3003,
	BOARD_APCI3002_16,
	BOARD_APCI3002_8,
	BOARD_APCI3002_4,
	BOARD_APCI3500,
};

struct apci3xxx_boardinfo {
	const char *name;
	int ai_subdev_flags;
	int ai_n_chan;
	unsigned int ai_maxdata;
	unsigned char ai_conv_units;
	unsigned int ai_min_acq_ns;
	unsigned int has_ao:1;
	unsigned int has_dig_in:1;
	unsigned int has_dig_out:1;
	unsigned int has_ttl_io:1;
};

static const struct apci3xxx_boardinfo apci3xxx_boardtypes[] = {
	[BOARD_APCI3000_16] = {
		.name			= "apci3000-16",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3000_8] = {
		.name			= "apci3000-8",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3000_4] = {
		.name			= "apci3000-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3006_16] = {
		.name			= "apci3006-16",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3006_8] = {
		.name			= "apci3006-8",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3006_4] = {
		.name			= "apci3006-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3010_16] = {
		.name			= "apci3010-16",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3010_8] = {
		.name			= "apci3010-8",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3010_4] = {
		.name			= "apci3010-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3016_16] = {
		.name			= "apci3016-16",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3016_8] = {
		.name			= "apci3016-8",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3016_4] = {
		.name			= "apci3016-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3100_16_4] = {
		.name			= "apci3100-16-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ao			= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3100_8_4] = {
		.name			= "apci3100-8-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ao			= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3106_16_4] = {
		.name			= "apci3106-16-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ao			= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3106_8_4] = {
		.name			= "apci3106-8-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 10000,
		.has_ao			= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3110_16_4] = {
		.name			= "apci3110-16-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_ao			= 1,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3110_8_4] = {
		.name			= "apci3110-8-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0x0fff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_ao			= 1,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3116_16_4] = {
		.name			= "apci3116-16-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_ao			= 1,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3116_8_4] = {
		.name			= "apci3116-8-4",
		.ai_subdev_flags	= SDF_COMMON | SDF_GROUND | SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_ao			= 1,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
		.has_ttl_io		= 1,
	},
	[BOARD_APCI3003] = {
		.name			= "apci3003",
		.ai_subdev_flags	= SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US |
					  CONV_UNIT_NS,
		.ai_min_acq_ns		= 2500,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
	},
	[BOARD_APCI3002_16] = {
		.name			= "apci3002-16",
		.ai_subdev_flags	= SDF_DIFF,
		.ai_n_chan		= 16,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
	},
	[BOARD_APCI3002_8] = {
		.name			= "apci3002-8",
		.ai_subdev_flags	= SDF_DIFF,
		.ai_n_chan		= 8,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
	},
	[BOARD_APCI3002_4] = {
		.name			= "apci3002-4",
		.ai_subdev_flags	= SDF_DIFF,
		.ai_n_chan		= 4,
		.ai_maxdata		= 0xffff,
		.ai_conv_units		= CONV_UNIT_MS | CONV_UNIT_US,
		.ai_min_acq_ns		= 5000,
		.has_dig_in		= 1,
		.has_dig_out		= 1,
	},
	[BOARD_APCI3500] = {
		.name			= "apci3500",
		.has_ao			= 1,
		.has_ttl_io		= 1,
	},
};

struct apci3xxx_private {
	void __iomem *mmio;
	unsigned int ui_AiNbrofChannels;	/*  how many channels is measured */
	unsigned int ui_AiReadData[32];
	unsigned char b_EocEosInterrupt;
	unsigned int ui_EocEosConversionTime;
	unsigned char b_EocEosConversionTimeBase;
	unsigned char b_SingelDiff;
};

#include "addi-data/hwdrv_apci3xxx.c"

static irqreturn_t apci3xxx_irq_handler(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct apci3xxx_private *devpriv = dev->private;
	unsigned int status;
	int i;

	/* Test if interrupt occur */
	status = readl(devpriv->mmio + 16);
	if ((status & 0x2) == 0x2) {
		/* Reset the interrupt */
		writel(status, devpriv->mmio + 16);

		/* Test if interrupt enabled */
		if (devpriv->b_EocEosInterrupt == 1) {
			/* Read all analog inputs value */
			for (i = 0; i < devpriv->ui_AiNbrofChannels; i++) {
				unsigned int val;

				val = readl(devpriv->mmio + 28);
				devpriv->ui_AiReadData[i] = val;
			}

			/* Set the interrupt flag */
			devpriv->b_EocEosInterrupt = 2;

			/* FIXME: comedi_event() */
		}
	}
	return IRQ_RETVAL(1);
}

static int apci3xxx_ao_insn_write(struct comedi_device *dev,
				  struct comedi_subdevice *s,
				  struct comedi_insn *insn,
				  unsigned int *data)
{
	struct apci3xxx_private *devpriv = dev->private;
	unsigned int chan = CR_CHAN(insn->chanspec);
	unsigned int range = CR_RANGE(insn->chanspec);
	unsigned int status;
	int i;

	for (i = 0; i < insn->n; i++) {
		/* Set the range selection */
		writel(range, devpriv->mmio + 96);

		/* Write the analog value to the selected channel */
		writel((data[i] << 8) | chan, devpriv->mmio + 100);

		/* Wait the end of transfer */
		do {
			status = readl(devpriv->mmio + 96);
		} while ((status & 0x100) != 0x100);
	}

	return insn->n;
}

static int apci3xxx_di_insn_bits(struct comedi_device *dev,
				 struct comedi_subdevice *s,
				 struct comedi_insn *insn,
				 unsigned int *data)
{
	data[1] = inl(dev->iobase + 32) & 0xf;

	return insn->n;
}

static int apci3xxx_do_insn_bits(struct comedi_device *dev,
				 struct comedi_subdevice *s,
				 struct comedi_insn *insn,
				 unsigned int *data)
{
	unsigned int mask = data[0];
	unsigned int bits = data[1];

	s->state = inl(dev->iobase + 48) & 0xf;
	if (mask) {
		s->state &= ~mask;
		s->state |= (bits & mask);

		outl(s->state, dev->iobase + 48);
	}

	data[1] = s->state;

	return insn->n;
}

static int apci3xxx_dio_insn_config(struct comedi_device *dev,
				    struct comedi_subdevice *s,
				    struct comedi_insn *insn,
				    unsigned int *data)
{
	unsigned int chan = CR_CHAN(insn->chanspec);
	unsigned int mask = 1 << chan;
	unsigned int bits;

	/*
	 * Port 0 (channels 0-7) are always inputs
	 * Port 1 (channels 8-15) are always outputs
	 * Port 2 (channels 16-23) are programmable i/o
	 *
	 * Changing any channel in port 2 changes the entire port.
	 */
	if (mask & 0xff0000)
		bits = 0xff0000;
	else
		bits = 0;

	switch (data[0]) {
	case INSN_CONFIG_DIO_INPUT:
		s->io_bits &= ~bits;
		break;
	case INSN_CONFIG_DIO_OUTPUT:
		s->io_bits |= bits;
		break;
	case INSN_CONFIG_DIO_QUERY:
		data[1] = (s->io_bits & bits) ? COMEDI_OUTPUT : COMEDI_INPUT;
		return insn->n;
	default:
		return -EINVAL;
	}

	/* update port 2 configuration */
	if (bits)
		outl((s->io_bits >> 24) & 0xff, dev->iobase + 224);

	return insn->n;
}

static int apci3xxx_dio_insn_bits(struct comedi_device *dev,
				  struct comedi_subdevice *s,
				  struct comedi_insn *insn,
				  unsigned int *data)
{
	unsigned int mask = data[0];
	unsigned int bits = data[1];
	unsigned int val;

	/* only update output channels */
	mask &= s->io_bits;
	if (mask) {
		s->state &= ~mask;
		s->state |= (bits & mask);

		if (mask & 0xff)
			outl(s->state & 0xff, dev->iobase + 80);
		if (mask & 0xff0000)
			outl((s->state >> 16) & 0xff, dev->iobase + 112);
	}

	val = inl(dev->iobase + 80);
	val |= (inl(dev->iobase + 64) << 8);
	if (s->io_bits & 0xff0000)
		val |= (inl(dev->iobase + 112) << 16);
	else
		val |= (inl(dev->iobase + 96) << 16);

	data[1] = val;

	return insn->n;
}

static int apci3xxx_reset(struct comedi_device *dev)
{
	struct apci3xxx_private *devpriv = dev->private;
	unsigned int val;
	int i;

	/* Disable the interrupt */
	disable_irq(dev->irq);

	/* Reset the interrupt flag */
	devpriv->b_EocEosInterrupt = 0;

	/* Clear the start command */
	writel(0, devpriv->mmio + 8);

	/* Reset the interrupt flags */
	val = readl(devpriv->mmio + 16);
	writel(val, devpriv->mmio + 16);

	/* clear the EOS */
	readl(devpriv->mmio + 20);

	/* Clear the FIFO */
	for (i = 0; i < 16; i++)
		val = readl(devpriv->mmio + 28);

	/* Enable the interrupt */
	enable_irq(dev->irq);

	return 0;
}

static int apci3xxx_auto_attach(struct comedi_device *dev,
				unsigned long context)
{
	struct pci_dev *pcidev = comedi_to_pci_dev(dev);
	const struct apci3xxx_boardinfo *board = NULL;
	struct apci3xxx_private *devpriv;
	struct comedi_subdevice *s;
	int ret, n_subdevices;

	if (context < ARRAY_SIZE(apci3xxx_boardtypes))
		board = &apci3xxx_boardtypes[context];
	if (!board)
		return -ENODEV;
	dev->board_ptr = board;
	dev->board_name = board->name;

	devpriv = kzalloc(sizeof(*devpriv), GFP_KERNEL);
	if (!devpriv)
		return -ENOMEM;
	dev->private = devpriv;

	ret = comedi_pci_enable(dev);
	if (ret)
		return ret;

	dev->iobase = pci_resource_start(pcidev, 2);
	devpriv->mmio = pci_ioremap_bar(pcidev, 3);

	if (pcidev->irq > 0) {
		ret = request_irq(pcidev->irq, apci3xxx_irq_handler,
				  IRQF_SHARED, dev->board_name, dev);
		if (ret == 0)
			dev->irq = pcidev->irq;
	}

	n_subdevices = 7;
	ret = comedi_alloc_subdevices(dev, n_subdevices);
	if (ret)
		return ret;

	/*  Allocate and Initialise AI Subdevice Structures */
	s = &dev->subdevices[0];
	if (board->ai_n_chan) {
		dev->read_subdev = s;
		s->type = COMEDI_SUBD_AI;
		s->subdev_flags = SDF_READABLE | board->ai_subdev_flags;
		s->n_chan = board->ai_n_chan;
		s->maxdata = board->ai_maxdata;
		s->len_chanlist = s->n_chan;
		s->range_table = &apci3xxx_ai_range;
		s->insn_config = apci3xxx_ai_insn_config;
		s->insn_read = apci3xxx_ai_insn_read;

		if ((board->ai_subdev_flags & (SDF_COMMON | SDF_GROUND)) == 0)
			devpriv->b_SingelDiff = 1;

	} else {
		s->type = COMEDI_SUBD_UNUSED;
	}

	/* Analog Output subdevice */
	s = &dev->subdevices[1];
	if (board->has_ao) {
		s->type		= COMEDI_SUBD_AO;
		s->subdev_flags	= SDF_WRITEABLE | SDF_GROUND | SDF_COMMON;
		s->n_chan	= 4;
		s->maxdata	= 0x0fff;
		s->range_table	= &apci3xxx_ao_range;
		s->insn_write	= apci3xxx_ao_insn_write;
	} else {
		s->type		= COMEDI_SUBD_UNUSED;
	}

	/* Digital Input subdevice */
	s = &dev->subdevices[2];
	if (board->has_dig_in) {
		s->type		= COMEDI_SUBD_DI;
		s->subdev_flags	= SDF_READABLE;
		s->n_chan	= 4;
		s->maxdata	= 1;
		s->range_table	= &range_digital;
		s->insn_bits	= apci3xxx_di_insn_bits;
	} else {
		s->type		= COMEDI_SUBD_UNUSED;
	}

	/* Digital Output subdevice */
	s = &dev->subdevices[3];
	if (board->has_dig_out) {
		s->type		= COMEDI_SUBD_DO;
		s->subdev_flags	= SDF_WRITEABLE;
		s->n_chan	= 4;
		s->maxdata	= 1;
		s->range_table	= &range_digital;
		s->insn_bits	= apci3xxx_do_insn_bits;
	} else {
		s->type		= COMEDI_SUBD_UNUSED;
	}

	/*  Allocate and Initialise Timer Subdevice Structures */
	s = &dev->subdevices[4];
	s->type = COMEDI_SUBD_UNUSED;

	/* TTL Digital I/O subdevice */
	s = &dev->subdevices[5];
	if (board->has_ttl_io) {
		s->type		= COMEDI_SUBD_DIO;
		s->subdev_flags	= SDF_READABLE | SDF_WRITEABLE;
		s->n_chan	= 24;
		s->maxdata	= 1;
		s->io_bits	= 0xff;	/* channels 0-7 are always outputs */
		s->range_table	= &range_digital;
		s->insn_config	= apci3xxx_dio_insn_config;
		s->insn_bits	= apci3xxx_dio_insn_bits;
	} else {
		s->type = COMEDI_SUBD_UNUSED;
	}

	/* EEPROM */
	s = &dev->subdevices[6];
	s->type = COMEDI_SUBD_UNUSED;

	apci3xxx_reset(dev);
	return 0;
}

static void apci3xxx_detach(struct comedi_device *dev)
{
	struct apci3xxx_private *devpriv = dev->private;

	if (devpriv) {
		if (dev->iobase)
			apci3xxx_reset(dev);
		if (dev->irq)
			free_irq(dev->irq, dev);
		if (devpriv->mmio)
			iounmap(devpriv->mmio);
	}
	comedi_pci_disable(dev);
}

static struct comedi_driver apci3xxx_driver = {
	.driver_name	= "addi_apci_3xxx",
	.module		= THIS_MODULE,
	.auto_attach	= apci3xxx_auto_attach,
	.detach		= apci3xxx_detach,
};

static int apci3xxx_pci_probe(struct pci_dev *dev,
			      const struct pci_device_id *id)
{
	return comedi_pci_auto_config(dev, &apci3xxx_driver, id->driver_data);
}

static DEFINE_PCI_DEVICE_TABLE(apci3xxx_pci_table) = {
	{ PCI_VDEVICE(ADDIDATA, 0x3010), BOARD_APCI3000_16 },
	{ PCI_VDEVICE(ADDIDATA, 0x300f), BOARD_APCI3000_8 },
	{ PCI_VDEVICE(ADDIDATA, 0x300e), BOARD_APCI3000_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3013), BOARD_APCI3006_16 },
	{ PCI_VDEVICE(ADDIDATA, 0x3014), BOARD_APCI3006_8 },
	{ PCI_VDEVICE(ADDIDATA, 0x3015), BOARD_APCI3006_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3016), BOARD_APCI3010_16 },
	{ PCI_VDEVICE(ADDIDATA, 0x3017), BOARD_APCI3010_8 },
	{ PCI_VDEVICE(ADDIDATA, 0x3018), BOARD_APCI3010_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3019), BOARD_APCI3016_16 },
	{ PCI_VDEVICE(ADDIDATA, 0x301a), BOARD_APCI3016_8 },
	{ PCI_VDEVICE(ADDIDATA, 0x301b), BOARD_APCI3016_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x301c), BOARD_APCI3100_16_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x301d), BOARD_APCI3100_8_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x301e), BOARD_APCI3106_16_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x301f), BOARD_APCI3106_8_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3020), BOARD_APCI3110_16_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3021), BOARD_APCI3110_8_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3022), BOARD_APCI3116_16_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3023), BOARD_APCI3116_8_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x300B), BOARD_APCI3003 },
	{ PCI_VDEVICE(ADDIDATA, 0x3002), BOARD_APCI3002_16 },
	{ PCI_VDEVICE(ADDIDATA, 0x3003), BOARD_APCI3002_8 },
	{ PCI_VDEVICE(ADDIDATA, 0x3004), BOARD_APCI3002_4 },
	{ PCI_VDEVICE(ADDIDATA, 0x3024), BOARD_APCI3500 },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, apci3xxx_pci_table);

static struct pci_driver apci3xxx_pci_driver = {
	.name		= "addi_apci_3xxx",
	.id_table	= apci3xxx_pci_table,
	.probe		= apci3xxx_pci_probe,
	.remove		= comedi_pci_auto_unconfig,
};
module_comedi_pci_driver(apci3xxx_driver, apci3xxx_pci_driver);

MODULE_AUTHOR("Comedi http://www.comedi.org");
MODULE_DESCRIPTION("Comedi low-level driver");
MODULE_LICENSE("GPL");
