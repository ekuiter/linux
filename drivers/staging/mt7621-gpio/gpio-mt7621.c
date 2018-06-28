// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MTK_BANK_CNT		3
#define MTK_BANK_WIDTH		32
#define PIN_MASK(nr)		(1UL << ((nr % MTK_BANK_WIDTH)))

#define GPIO_BANK_WIDE	0x04
#define GPIO_REG_CTRL	0x00
#define GPIO_REG_POL	0x10
#define GPIO_REG_DATA	0x20
#define GPIO_REG_DSET	0x30
#define GPIO_REG_DCLR	0x40
#define GPIO_REG_REDGE	0x50
#define GPIO_REG_FEDGE	0x60
#define GPIO_REG_HLVL	0x70
#define GPIO_REG_LLVL	0x80
#define GPIO_REG_STAT	0x90
#define GPIO_REG_EDGE	0xA0

struct mtk_gc {
	struct gpio_chip chip;
	spinlock_t lock;
	int bank;
	u32 rising;
	u32 falling;
};

struct mtk_data {
	void __iomem *gpio_membase;
	int gpio_irq;
	struct irq_domain *gpio_irq_domain;
	struct mtk_gc gc_map[MTK_BANK_CNT];
};

static inline struct mtk_gc *
to_mediatek_gpio(struct gpio_chip *chip)
{
	return container_of(chip, struct mtk_gc, chip);
}

static inline void
mtk_gpio_w32(struct mtk_gc *rg, u32 offset, u32 val)
{
	struct gpio_chip *gc = &rg->chip;
	struct mtk_data *gpio_data = gpiochip_get_data(gc);

	offset = (rg->bank * GPIO_BANK_WIDE) + offset;
	gc->write_reg(gpio_data->gpio_membase + offset, val);
}

static inline u32
mtk_gpio_r32(struct mtk_gc *rg, u32 offset)
{
	struct gpio_chip *gc = &rg->chip;
	struct mtk_data *gpio_data = gpiochip_get_data(gc);

	offset = (rg->bank * GPIO_BANK_WIDE) + offset;
	return gc->read_reg(gpio_data->gpio_membase + offset);
}

static int
mediatek_gpio_to_irq(struct gpio_chip *chip, unsigned int pin)
{
	struct mtk_data *gpio_data = gpiochip_get_data(chip);
	struct mtk_gc *rg = to_mediatek_gpio(chip);

	return irq_create_mapping(gpio_data->gpio_irq_domain,
				  pin + (rg->bank * MTK_BANK_WIDTH));
}

static int
mediatek_gpio_bank_probe(struct platform_device *pdev, struct device_node *bank)
{
	struct mtk_data *gpio = dev_get_drvdata(&pdev->dev);
	const __be32 *id = of_get_property(bank, "reg", NULL);
	struct mtk_gc *rg;
	void __iomem *dat, *set, *ctrl, *diro;
	int ret;

	if (!id || be32_to_cpu(*id) >= MTK_BANK_CNT)
		return -EINVAL;

	rg = &gpio->gc_map[be32_to_cpu(*id)];
	memset(rg, 0, sizeof(*rg));

	spin_lock_init(&rg->lock);
	rg->chip.of_node = bank;
	rg->bank = be32_to_cpu(*id);

	dat = gpio->gpio_membase + GPIO_REG_DATA + (rg->bank * GPIO_BANK_WIDE);
	set = gpio->gpio_membase + GPIO_REG_DSET + (rg->bank * GPIO_BANK_WIDE);
	ctrl = gpio->gpio_membase + GPIO_REG_DCLR + (rg->bank * GPIO_BANK_WIDE);
	diro = gpio->gpio_membase + GPIO_REG_CTRL + (rg->bank * GPIO_BANK_WIDE);

	ret = bgpio_init(&rg->chip, &pdev->dev, 4,
			 dat, set, ctrl, diro, NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "bgpio_init() failed\n");
		return ret;
	}

	if (gpio->gpio_irq_domain)
		rg->chip.to_irq = mediatek_gpio_to_irq;

	ret = devm_gpiochip_add_data(&pdev->dev, &rg->chip, gpio);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not register gpio %d, ret=%d\n",
			rg->chip.ngpio, ret);
		return ret;
	}

	/* set polarity to low for all gpios */
	mtk_gpio_w32(rg, GPIO_REG_POL, 0);

	dev_info(&pdev->dev, "registering %d gpios\n", rg->chip.ngpio);

	return 0;
}

static void
mediatek_gpio_irq_handler(struct irq_desc *desc)
{
	struct mtk_data *gpio_data = irq_desc_get_handler_data(desc);
	int i;

	for (i = 0; i < MTK_BANK_CNT; i++) {
		struct mtk_gc *rg = &gpio_data->gc_map[i];
		unsigned long pending;
		int bit;

		if (!rg)
			continue;

		pending = mtk_gpio_r32(rg, GPIO_REG_STAT);

		for_each_set_bit(bit, &pending, MTK_BANK_WIDTH) {
			u32 map = irq_find_mapping(gpio_data->gpio_irq_domain,
						   (MTK_BANK_WIDTH * i) + bit);

			generic_handle_irq(map);
			mtk_gpio_w32(rg, GPIO_REG_STAT, BIT(bit));
		}
	}
}

static void
mediatek_gpio_irq_unmask(struct irq_data *d)
{
	struct mtk_data *gpio_data = irq_data_get_irq_chip_data(d);
	int pin = d->hwirq;
	int bank = pin / MTK_BANK_WIDTH;
	struct mtk_gc *rg = &gpio_data->gc_map[bank];
	unsigned long flags;
	u32 rise, fall;

	if (!rg)
		return;

	spin_lock_irqsave(&rg->lock, flags);
	rise = mtk_gpio_r32(rg, GPIO_REG_REDGE);
	fall = mtk_gpio_r32(rg, GPIO_REG_FEDGE);
	mtk_gpio_w32(rg, GPIO_REG_REDGE, rise | (PIN_MASK(pin) & rg->rising));
	mtk_gpio_w32(rg, GPIO_REG_FEDGE, fall | (PIN_MASK(pin) & rg->falling));
	spin_unlock_irqrestore(&rg->lock, flags);
}

static void
mediatek_gpio_irq_mask(struct irq_data *d)
{
	struct mtk_data *gpio_data = irq_data_get_irq_chip_data(d);
	int pin = d->hwirq;
	int bank = pin / MTK_BANK_WIDTH;
	struct mtk_gc *rg = &gpio_data->gc_map[bank];
	unsigned long flags;
	u32 rise, fall;

	if (!rg)
		return;

	spin_lock_irqsave(&rg->lock, flags);
	rise = mtk_gpio_r32(rg, GPIO_REG_REDGE);
	fall = mtk_gpio_r32(rg, GPIO_REG_FEDGE);
	mtk_gpio_w32(rg, GPIO_REG_FEDGE, fall & ~PIN_MASK(pin));
	mtk_gpio_w32(rg, GPIO_REG_REDGE, rise & ~PIN_MASK(pin));
	spin_unlock_irqrestore(&rg->lock, flags);
}

static int
mediatek_gpio_irq_type(struct irq_data *d, unsigned int type)
{
	struct mtk_data *gpio_data = irq_data_get_irq_chip_data(d);
	int pin = d->hwirq;
	int bank = pin / MTK_BANK_WIDTH;
	struct mtk_gc *rg = &gpio_data->gc_map[bank];
	u32 mask = PIN_MASK(pin);

	if (!rg)
		return -1;

	if (type == IRQ_TYPE_PROBE) {
		if ((rg->rising | rg->falling) & mask)
			return 0;

		type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	}

	if (type & IRQ_TYPE_EDGE_RISING)
		rg->rising |= mask;
	else
		rg->rising &= ~mask;

	if (type & IRQ_TYPE_EDGE_FALLING)
		rg->falling |= mask;
	else
		rg->falling &= ~mask;

	return 0;
}

static struct irq_chip mediatek_gpio_irq_chip = {
	.name		= "GPIO",
	.irq_unmask	= mediatek_gpio_irq_unmask,
	.irq_mask	= mediatek_gpio_irq_mask,
	.irq_mask_ack	= mediatek_gpio_irq_mask,
	.irq_set_type	= mediatek_gpio_irq_type,
};

static int
mediatek_gpio_gpio_map(struct irq_domain *d, unsigned int irq,
		       irq_hw_number_t hw)
{
	int ret;

	ret = irq_set_chip_data(irq, d->host_data);
	if (ret < 0)
		return ret;
	irq_set_chip_and_handler(irq, &mediatek_gpio_irq_chip,
				 handle_level_irq);
	irq_set_handler_data(irq, d);

	return 0;
}

static const struct irq_domain_ops irq_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
	.map = mediatek_gpio_gpio_map,
};

static int
mediatek_gpio_probe(struct platform_device *pdev)
{
	struct device_node *bank, *np = pdev->dev.of_node;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct mtk_data *gpio_data;

	gpio_data = devm_kzalloc(&pdev->dev, sizeof(*gpio_data), GFP_KERNEL);
	if (!gpio_data)
		return -ENOMEM;

	gpio_data->gpio_membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gpio_data->gpio_membase))
		return PTR_ERR(gpio_data->gpio_membase);

	gpio_data->gpio_irq = irq_of_parse_and_map(np, 0);
	if (gpio_data->gpio_irq) {
		gpio_data->gpio_irq_domain = irq_domain_add_linear(np,
			MTK_BANK_CNT * MTK_BANK_WIDTH,
			&irq_domain_ops, gpio_data);
		if (!gpio_data->gpio_irq_domain)
			dev_err(&pdev->dev, "irq_domain_add_linear failed\n");
	}

	platform_set_drvdata(pdev, gpio_data);

	for_each_child_of_node(np, bank)
		if (of_device_is_compatible(bank, "mediatek,mt7621-gpio-bank"))
			mediatek_gpio_bank_probe(pdev, bank);

	if (gpio_data->gpio_irq_domain)
		irq_set_chained_handler_and_data(gpio_data->gpio_irq,
						 mediatek_gpio_irq_handler,
						 gpio_data);

	return 0;
}

static const struct of_device_id mediatek_gpio_match[] = {
	{ .compatible = "mediatek,mt7621-gpio" },
	{},
};
MODULE_DEVICE_TABLE(of, mediatek_gpio_match);

static struct platform_driver mediatek_gpio_driver = {
	.probe = mediatek_gpio_probe,
	.driver = {
		.name = "mt7621_gpio",
		.of_match_table = mediatek_gpio_match,
	},
};

builtin_platform_driver(mediatek_gpio_driver);
