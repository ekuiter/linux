// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/devfreq-event.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

#include <soc/rockchip/rockchip_grf.h>
#include <soc/rockchip/rk3399_grf.h>
#include <soc/rockchip/rk3568_grf.h>

#define DMC_MAX_CHANNELS	2

#define HIWORD_UPDATE(val, mask)	((val) | (mask) << 16)

/* DDRMON_CTRL */
#define DDRMON_CTRL	0x04
#define DDRMON_CTRL_DDR4		BIT(5)
#define DDRMON_CTRL_LPDDR4		BIT(4)
#define DDRMON_CTRL_HARDWARE_EN		BIT(3)
#define DDRMON_CTRL_LPDDR23		BIT(2)
#define DDRMON_CTRL_SOFTWARE_EN		BIT(1)
#define DDRMON_CTRL_TIMER_CNT_EN	BIT(0)
#define DDRMON_CTRL_DDR_TYPE_MASK	(DDRMON_CTRL_DDR4 | \
					 DDRMON_CTRL_LPDDR4 | \
					 DDRMON_CTRL_LPDDR23)

#define DDRMON_CH0_COUNT_NUM		0x28
#define DDRMON_CH0_DFI_ACCESS_NUM	0x2c
#define DDRMON_CH1_COUNT_NUM		0x3c
#define DDRMON_CH1_DFI_ACCESS_NUM	0x40

struct dmc_count_channel {
	u32 access;
	u32 total;
};

struct dmc_count {
	struct dmc_count_channel c[DMC_MAX_CHANNELS];
};

/*
 * The dfi controller can monitor DDR load. It has an upper and lower threshold
 * for the operating points. Whenever the usage leaves these bounds an event is
 * generated to indicate the DDR frequency should be changed.
 */
struct rockchip_dfi {
	struct devfreq_event_dev *edev;
	struct devfreq_event_desc desc;
	struct dmc_count last_event_count;
	struct device *dev;
	void __iomem *regs;
	struct regmap *regmap_pmu;
	struct clk *clk;
	u32 ddr_type;
	unsigned int channel_mask;
	unsigned int max_channels;
};

static void rockchip_dfi_start_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = dfi->regs;

	/* clear DDRMON_CTRL setting */
	writel_relaxed(HIWORD_UPDATE(0, DDRMON_CTRL_TIMER_CNT_EN | DDRMON_CTRL_SOFTWARE_EN |
		       DDRMON_CTRL_HARDWARE_EN), dfi_regs + DDRMON_CTRL);

	/* set ddr type to dfi */
	switch (dfi->ddr_type) {
	case ROCKCHIP_DDRTYPE_LPDDR2:
	case ROCKCHIP_DDRTYPE_LPDDR3:
		writel_relaxed(HIWORD_UPDATE(DDRMON_CTRL_LPDDR23, DDRMON_CTRL_DDR_TYPE_MASK),
			       dfi_regs + DDRMON_CTRL);
		break;
	case ROCKCHIP_DDRTYPE_LPDDR4:
		writel_relaxed(HIWORD_UPDATE(DDRMON_CTRL_LPDDR4, DDRMON_CTRL_DDR_TYPE_MASK),
			       dfi_regs + DDRMON_CTRL);
		break;
	default:
		break;
	}

	/* enable count, use software mode */
	writel_relaxed(HIWORD_UPDATE(DDRMON_CTRL_SOFTWARE_EN, DDRMON_CTRL_SOFTWARE_EN),
		       dfi_regs + DDRMON_CTRL);
}

static void rockchip_dfi_stop_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = dfi->regs;

	writel_relaxed(HIWORD_UPDATE(0, DDRMON_CTRL_SOFTWARE_EN),
		       dfi_regs + DDRMON_CTRL);
}

static void rockchip_dfi_read_counters(struct devfreq_event_dev *edev, struct dmc_count *count)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);
	u32 i;
	void __iomem *dfi_regs = dfi->regs;

	for (i = 0; i < dfi->max_channels; i++) {
		if (!(dfi->channel_mask & BIT(i)))
			continue;
		count->c[i].access = readl_relaxed(dfi_regs +
				DDRMON_CH0_DFI_ACCESS_NUM + i * 20);
		count->c[i].total = readl_relaxed(dfi_regs +
				DDRMON_CH0_COUNT_NUM + i * 20);
	}
}

static int rockchip_dfi_disable(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);

	rockchip_dfi_stop_hardware_counter(edev);
	clk_disable_unprepare(dfi->clk);

	return 0;
}

static int rockchip_dfi_enable(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);
	int ret;

	ret = clk_prepare_enable(dfi->clk);
	if (ret) {
		dev_err(&edev->dev, "failed to enable dfi clk: %d\n", ret);
		return ret;
	}

	rockchip_dfi_start_hardware_counter(edev);
	return 0;
}

static int rockchip_dfi_set_event(struct devfreq_event_dev *edev)
{
	return 0;
}

static int rockchip_dfi_get_event(struct devfreq_event_dev *edev,
				  struct devfreq_event_data *edata)
{
	struct rockchip_dfi *dfi = devfreq_event_get_drvdata(edev);
	struct dmc_count count;
	struct dmc_count *last = &dfi->last_event_count;
	u32 access = 0, total = 0;
	int i;

	rockchip_dfi_read_counters(edev, &count);

	/* We can only report one channel, so find the busiest one */
	for (i = 0; i < dfi->max_channels; i++) {
		u32 a, t;

		if (!(dfi->channel_mask & BIT(i)))
			continue;

		a = count.c[i].access - last->c[i].access;
		t = count.c[i].total - last->c[i].total;

		if (a > access) {
			access = a;
			total = t;
		}
	}

	edata->load_count = access * 4;
	edata->total_count = total;

	dfi->last_event_count = count;

	return 0;
}

static const struct devfreq_event_ops rockchip_dfi_ops = {
	.disable = rockchip_dfi_disable,
	.enable = rockchip_dfi_enable,
	.get_event = rockchip_dfi_get_event,
	.set_event = rockchip_dfi_set_event,
};

static int rk3399_dfi_init(struct rockchip_dfi *dfi)
{
	struct regmap *regmap_pmu = dfi->regmap_pmu;
	u32 val;

	dfi->clk = devm_clk_get(dfi->dev, "pclk_ddr_mon");
	if (IS_ERR(dfi->clk))
		return dev_err_probe(dfi->dev, PTR_ERR(dfi->clk),
				     "Cannot get the clk pclk_ddr_mon\n");

	/* get ddr type */
	regmap_read(regmap_pmu, RK3399_PMUGRF_OS_REG2, &val);
	dfi->ddr_type = FIELD_GET(RK3399_PMUGRF_OS_REG2_DDRTYPE, val);

	dfi->channel_mask = GENMASK(1, 0);
	dfi->max_channels = 2;

	return 0;
};

static int rk3568_dfi_init(struct rockchip_dfi *dfi)
{
	struct regmap *regmap_pmu = dfi->regmap_pmu;
	u32 reg2, reg3;

	regmap_read(regmap_pmu, RK3568_PMUGRF_OS_REG2, &reg2);
	regmap_read(regmap_pmu, RK3568_PMUGRF_OS_REG3, &reg3);

	/* lower 3 bits of the DDR type */
	dfi->ddr_type = FIELD_GET(RK3568_PMUGRF_OS_REG2_DRAMTYPE_INFO, reg2);

	/*
	 * For version three and higher the upper two bits of the DDR type are
	 * in RK3568_PMUGRF_OS_REG3
	 */
	if (FIELD_GET(RK3568_PMUGRF_OS_REG3_SYSREG_VERSION, reg3) >= 0x3)
		dfi->ddr_type |= FIELD_GET(RK3568_PMUGRF_OS_REG3_DRAMTYPE_INFO_V3, reg3) << 3;

	dfi->channel_mask = BIT(0);
	dfi->max_channels = 1;

	return 0;
};

static const struct of_device_id rockchip_dfi_id_match[] = {
	{ .compatible = "rockchip,rk3399-dfi", .data = rk3399_dfi_init },
	{ .compatible = "rockchip,rk3568-dfi", .data = rk3568_dfi_init },
	{ },
};

MODULE_DEVICE_TABLE(of, rockchip_dfi_id_match);

static int rockchip_dfi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_dfi *dfi;
	struct devfreq_event_desc *desc;
	struct device_node *np = pdev->dev.of_node, *node;
	int (*soc_init)(struct rockchip_dfi *dfi);
	int ret;

	soc_init = of_device_get_match_data(&pdev->dev);
	if (!soc_init)
		return -EINVAL;

	dfi = devm_kzalloc(dev, sizeof(*dfi), GFP_KERNEL);
	if (!dfi)
		return -ENOMEM;

	dfi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dfi->regs))
		return PTR_ERR(dfi->regs);

	node = of_parse_phandle(np, "rockchip,pmu", 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -ENODEV, "Can't find pmu_grf registers\n");

	dfi->regmap_pmu = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(dfi->regmap_pmu))
		return PTR_ERR(dfi->regmap_pmu);

	dfi->dev = dev;

	desc = &dfi->desc;
	desc->ops = &rockchip_dfi_ops;
	desc->driver_data = dfi;
	desc->name = np->name;

	ret = soc_init(dfi);
	if (ret)
		return ret;

	dfi->edev = devm_devfreq_event_add_edev(&pdev->dev, desc);
	if (IS_ERR(dfi->edev)) {
		dev_err(&pdev->dev,
			"failed to add devfreq-event device\n");
		return PTR_ERR(dfi->edev);
	}

	platform_set_drvdata(pdev, dfi);

	return 0;
}

static struct platform_driver rockchip_dfi_driver = {
	.probe	= rockchip_dfi_probe,
	.driver = {
		.name	= "rockchip-dfi",
		.of_match_table = rockchip_dfi_id_match,
	},
};
module_platform_driver(rockchip_dfi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lin Huang <hl@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip DFI driver");
