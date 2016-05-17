/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *	Mikko Perttunen <mperttunen@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>

#include <dt-bindings/thermal/tegra124-soctherm.h>

#include "soctherm.h"

#define SENSOR_CONFIG0				0
#define SENSOR_CONFIG0_STOP			BIT(0)
#define SENSOR_CONFIG0_CPTR_OVER		BIT(2)
#define SENSOR_CONFIG0_OVER			BIT(3)
#define SENSOR_CONFIG0_TCALC_OVER		BIT(4)
#define SENSOR_CONFIG0_TALL_MASK		(0xfffff << 8)
#define SENSOR_CONFIG0_TALL_SHIFT		8

#define SENSOR_CONFIG1				4
#define SENSOR_CONFIG1_TSAMPLE_MASK		0x3ff
#define SENSOR_CONFIG1_TSAMPLE_SHIFT		0
#define SENSOR_CONFIG1_TIDDQ_EN_MASK		(0x3f << 15)
#define SENSOR_CONFIG1_TIDDQ_EN_SHIFT		15
#define SENSOR_CONFIG1_TEN_COUNT_MASK		(0x3f << 24)
#define SENSOR_CONFIG1_TEN_COUNT_SHIFT		24
#define SENSOR_CONFIG1_TEMP_ENABLE		BIT(31)

/*
 * SENSOR_CONFIG2 is defined in soctherm.h
 * because, it will be used by tegra_soctherm_fuse.c
 */

#define SENSOR_STATUS0				0xc
#define SENSOR_STATUS0_VALID_MASK		BIT(31)
#define SENSOR_STATUS0_CAPTURE_MASK		0xffff

#define SENSOR_STATUS1				0x10
#define SENSOR_STATUS1_TEMP_VALID_MASK		BIT(31)
#define SENSOR_STATUS1_TEMP_MASK		0xffff

#define READBACK_VALUE_MASK			0xff00
#define READBACK_VALUE_SHIFT			8
#define READBACK_ADD_HALF			BIT(7)
#define READBACK_NEGATE				BIT(0)

/* get val from register(r) mask bits(m) */
#define REG_GET_MASK(r, m)	(((r) & (m)) >> (ffs(m) - 1))
/* set val(v) to mask bits(m) of register(r) */
#define REG_SET_MASK(r, m, v)	(((r) & ~(m)) | \
				 (((v) & (m >> (ffs(m) - 1))) << (ffs(m) - 1)))

struct tegra_thermctl_zone {
	void __iomem *reg;
	u32 mask;
};

struct tegra_soctherm {
	struct reset_control *reset;
	struct clk *clock_tsensor;
	struct clk *clock_soctherm;
	void __iomem *regs;

	u32 *calib;
	struct tegra_soctherm_soc *soc;

	struct dentry *debugfs_dir;
};

static int enable_tsensor(struct tegra_soctherm *tegra,
			  unsigned int i,
			  const struct tsensor_shared_calib *shared)
{
	const struct tegra_tsensor *sensor = &tegra->soc->tsensors[i];
	void __iomem *base = tegra->regs + sensor->base;
	u32 *calib = &tegra->calib[i];
	unsigned int val;
	int err;

	err = tegra_calc_tsensor_calib(sensor, shared, calib);
	if (err)
		return err;

	val = sensor->config->tall << SENSOR_CONFIG0_TALL_SHIFT;
	writel(val, base + SENSOR_CONFIG0);

	val  = (sensor->config->tsample - 1) << SENSOR_CONFIG1_TSAMPLE_SHIFT;
	val |= sensor->config->tiddq_en << SENSOR_CONFIG1_TIDDQ_EN_SHIFT;
	val |= sensor->config->ten_count << SENSOR_CONFIG1_TEN_COUNT_SHIFT;
	val |= SENSOR_CONFIG1_TEMP_ENABLE;
	writel(val, base + SENSOR_CONFIG1);

	writel(*calib, base + SENSOR_CONFIG2);

	return 0;
}

/*
 * Translate from soctherm readback format to millicelsius.
 * The soctherm readback format in bits is as follows:
 *   TTTTTTTT H______N
 * where T's contain the temperature in Celsius,
 * H denotes an addition of 0.5 Celsius and N denotes negation
 * of the final value.
 */
static int translate_temp(u16 val)
{
	int t;

	t = ((val & READBACK_VALUE_MASK) >> READBACK_VALUE_SHIFT) * 1000;
	if (val & READBACK_ADD_HALF)
		t += 500;
	if (val & READBACK_NEGATE)
		t *= -1;

	return t;
}

static int tegra_thermctl_get_temp(void *data, int *out_temp)
{
	struct tegra_thermctl_zone *zone = data;
	u32 val;

	val = readl(zone->reg);
	val = REG_GET_MASK(val, zone->mask);
	*out_temp = translate_temp(val);

	return 0;
}

static const struct thermal_zone_of_device_ops tegra_of_thermal_ops = {
	.get_temp = tegra_thermctl_get_temp,
};

#ifdef CONFIG_DEBUG_FS
static int regs_show(struct seq_file *s, void *data)
{
	struct platform_device *pdev = s->private;
	struct tegra_soctherm *ts = platform_get_drvdata(pdev);
	const struct tegra_tsensor *tsensors = ts->soc->tsensors;
	u32 r, state;
	int i;

	seq_puts(s, "-----TSENSE (convert HW)-----\n");

	for (i = 0; i < ts->soc->num_tsensors; i++) {
		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG1);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TEMP_ENABLE);

		seq_printf(s, "%s: ", tsensors[i].name);
		seq_printf(s, "En(%d) ", state);

		if (!state) {
			seq_puts(s, "\n");
			continue;
		}

		state = REG_GET_MASK(r, SENSOR_CONFIG1_TIDDQ_EN_MASK);
		seq_printf(s, "tiddq(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TEN_COUNT_MASK);
		seq_printf(s, "ten_count(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TSAMPLE_MASK);
		seq_printf(s, "tsample(%d) ", state + 1);

		r = readl(ts->regs + tsensors[i].base + SENSOR_STATUS1);
		state = REG_GET_MASK(r, SENSOR_STATUS1_TEMP_VALID_MASK);
		seq_printf(s, "Temp(%d/", state);
		state = REG_GET_MASK(r, SENSOR_STATUS1_TEMP_MASK);
		seq_printf(s, "%d) ", translate_temp(state));

		r = readl(ts->regs + tsensors[i].base + SENSOR_STATUS0);
		state = REG_GET_MASK(r, SENSOR_STATUS0_VALID_MASK);
		seq_printf(s, "Capture(%d/", state);
		state = REG_GET_MASK(r, SENSOR_STATUS0_CAPTURE_MASK);
		seq_printf(s, "%d) ", state);

		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG0);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_STOP);
		seq_printf(s, "Stop(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_TALL_MASK);
		seq_printf(s, "Tall(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_TCALC_OVER);
		seq_printf(s, "Over(%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_OVER);
		seq_printf(s, "%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_CPTR_OVER);
		seq_printf(s, "%d) ", state);

		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG2);
		state = REG_GET_MASK(r, SENSOR_CONFIG2_THERMA_MASK);
		seq_printf(s, "Therm_A/B(%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG2_THERMB_MASK);
		seq_printf(s, "%d)\n", (s16)state);
	}

	r = readl(ts->regs + SENSOR_PDIV);
	seq_printf(s, "PDIV: 0x%x\n", r);

	r = readl(ts->regs + SENSOR_HOTSPOT_OFF);
	seq_printf(s, "HOTSPOT: 0x%x\n", r);

	seq_puts(s, "\n");
	seq_puts(s, "-----SOC_THERM-----\n");

	r = readl(ts->regs + SENSOR_TEMP1);
	state = REG_GET_MASK(r, SENSOR_TEMP1_CPU_TEMP_MASK);
	seq_printf(s, "Temperatures: CPU(%d) ", translate_temp(state));
	state = REG_GET_MASK(r, SENSOR_TEMP1_GPU_TEMP_MASK);
	seq_printf(s, " GPU(%d) ", translate_temp(state));
	r = readl(ts->regs + SENSOR_TEMP2);
	state = REG_GET_MASK(r, SENSOR_TEMP2_PLLX_TEMP_MASK);
	seq_printf(s, " PLLX(%d) ", translate_temp(state));
	state = REG_GET_MASK(r, SENSOR_TEMP2_MEM_TEMP_MASK);
	seq_printf(s, " MEM(%d)\n", translate_temp(state));

	return 0;
}

static int regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, regs_show, inode->i_private);
}

static const struct file_operations regs_fops = {
	.open		= regs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void soctherm_debug_init(struct platform_device *pdev)
{
	struct tegra_soctherm *tegra = platform_get_drvdata(pdev);
	struct dentry *root, *file;

	root = debugfs_create_dir("soctherm", NULL);
	if (!root) {
		dev_err(&pdev->dev, "failed to create debugfs directory\n");
		return;
	}

	tegra->debugfs_dir = root;

	file = debugfs_create_file("reg_contents", 0644, root,
				   pdev, &regs_fops);
	if (!file) {
		dev_err(&pdev->dev, "failed to create debugfs file\n");
		debugfs_remove_recursive(tegra->debugfs_dir);
		tegra->debugfs_dir = NULL;
	}
}
#else
static inline void soctherm_debug_init(struct platform_device *pdev) {}
#endif

static const struct of_device_id tegra_soctherm_of_match[] = {
#ifdef CONFIG_ARCH_TEGRA_124_SOC
	{
		.compatible = "nvidia,tegra124-soctherm",
		.data = &tegra124_soctherm,
	},
#endif
#ifdef CONFIG_ARCH_TEGRA_210_SOC
	{
		.compatible = "nvidia,tegra210-soctherm",
		.data = &tegra210_soctherm,
	},
#endif
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_soctherm_of_match);

static int tegra_soctherm_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct tegra_soctherm *tegra;
	struct thermal_zone_device *z;
	struct tsensor_shared_calib shared_calib;
	struct resource *res;
	struct tegra_soctherm_soc *soc;
	unsigned int i;
	int err;
	u32 pdiv, hotspot;

	match = of_match_node(tegra_soctherm_of_match, pdev->dev.of_node);
	if (!match)
		return -ENODEV;

	soc = (struct tegra_soctherm_soc *)match->data;
	if (soc->num_ttgs > TEGRA124_SOCTHERM_SENSOR_NUM)
		return -EINVAL;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, tegra);

	tegra->soc = soc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tegra->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tegra->regs))
		return PTR_ERR(tegra->regs);

	tegra->reset = devm_reset_control_get(&pdev->dev, "soctherm");
	if (IS_ERR(tegra->reset)) {
		dev_err(&pdev->dev, "can't get soctherm reset\n");
		return PTR_ERR(tegra->reset);
	}

	tegra->clock_tsensor = devm_clk_get(&pdev->dev, "tsensor");
	if (IS_ERR(tegra->clock_tsensor)) {
		dev_err(&pdev->dev, "can't get tsensor clock\n");
		return PTR_ERR(tegra->clock_tsensor);
	}

	tegra->clock_soctherm = devm_clk_get(&pdev->dev, "soctherm");
	if (IS_ERR(tegra->clock_soctherm)) {
		dev_err(&pdev->dev, "can't get soctherm clock\n");
		return PTR_ERR(tegra->clock_soctherm);
	}

	reset_control_assert(tegra->reset);

	err = clk_prepare_enable(tegra->clock_soctherm);
	if (err)
		return err;

	err = clk_prepare_enable(tegra->clock_tsensor);
	if (err) {
		clk_disable_unprepare(tegra->clock_soctherm);
		return err;
	}

	reset_control_deassert(tegra->reset);

	/* Initialize raw sensors */

	tegra->calib = devm_kzalloc(&pdev->dev,
				    sizeof(u32) * soc->num_tsensors,
				    GFP_KERNEL);
	if (!tegra->calib) {
		err = -ENOMEM;
		goto disable_clocks;
	}

	err = tegra_calc_shared_calib(soc->tfuse, &shared_calib);
	if (err)
		goto disable_clocks;

	for (i = 0; i < soc->num_tsensors; ++i) {
		err = enable_tsensor(tegra, i, &shared_calib);
		if (err)
			goto disable_clocks;
	}

	/* Program pdiv and hotspot offsets per THERM */
	pdiv = readl(tegra->regs + SENSOR_PDIV);
	hotspot = readl(tegra->regs + SENSOR_HOTSPOT_OFF);
	for (i = 0; i < soc->num_ttgs; ++i) {
		pdiv = REG_SET_MASK(pdiv, soc->ttgs[i]->pdiv_mask,
				    soc->ttgs[i]->pdiv);
		/* hotspot offset from PLLX, doesn't need to configure PLLX */
		if (soc->ttgs[i]->id == TEGRA124_SOCTHERM_SENSOR_PLLX)
			continue;
		hotspot =  REG_SET_MASK(hotspot,
					soc->ttgs[i]->pllx_hotspot_mask,
					soc->ttgs[i]->pllx_hotspot_diff);
	}
	writel(pdiv, tegra->regs + SENSOR_PDIV);
	writel(hotspot, tegra->regs + SENSOR_HOTSPOT_OFF);

	/* Initialize thermctl sensors */

	for (i = 0; i < soc->num_ttgs; ++i) {
		struct tegra_thermctl_zone *zone =
			devm_kzalloc(&pdev->dev, sizeof(*zone), GFP_KERNEL);
		if (!zone) {
			err = -ENOMEM;
			goto disable_clocks;
		}

		zone->reg = tegra->regs + soc->ttgs[i]->sensor_temp_offset;
		zone->mask = soc->ttgs[i]->sensor_temp_mask;

		z = devm_thermal_zone_of_sensor_register(&pdev->dev,
							 soc->ttgs[i]->id, zone,
							 &tegra_of_thermal_ops);
		if (IS_ERR(z)) {
			err = PTR_ERR(z);
			dev_err(&pdev->dev, "failed to register sensor: %d\n",
				err);
			goto disable_clocks;
		}
	}

	soctherm_debug_init(pdev);

	return 0;

disable_clocks:
	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return err;
}

static int tegra_soctherm_remove(struct platform_device *pdev)
{
	struct tegra_soctherm *tegra = platform_get_drvdata(pdev);

	debugfs_remove_recursive(tegra->debugfs_dir);

	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return 0;
}

static struct platform_driver tegra_soctherm_driver = {
	.probe = tegra_soctherm_probe,
	.remove = tegra_soctherm_remove,
	.driver = {
		.name = "tegra_soctherm",
		.of_match_table = tegra_soctherm_of_match,
	},
};
module_platform_driver(tegra_soctherm_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra SOCTHERM thermal management driver");
MODULE_LICENSE("GPL v2");
