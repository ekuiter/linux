// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP
 */

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define OCOTP_CFG3_SPEED_GRADE_SHIFT	8
#define OCOTP_CFG3_SPEED_GRADE_MASK	(0x3 << 8)
#define OCOTP_CFG3_MKT_SEGMENT_SHIFT    6
#define OCOTP_CFG3_MKT_SEGMENT_MASK     (0x3 << 6)

static const struct of_device_id imx_cpufreq_dt_match_list[] = {
	{ .compatible = "fsl,imx8mm" },
	{ .compatible = "fsl,imx8mq" },
	{}
};

/* cpufreq-dt device registered by imx-cpufreq-dt */
static struct platform_device *cpufreq_dt_pdev;
static struct opp_table *cpufreq_opp_table;

static int imx_cpufreq_dt_probe(struct platform_device *pdev)
{
	struct device *cpu_dev = get_cpu_device(0);
	struct device_node *np;
	const struct of_device_id *match;
	u32 cell_value, supported_hw[2];
	int speed_grade, mkt_segment;
	int ret;

	np = of_find_node_by_path("/");
	match = of_match_node(imx_cpufreq_dt_match_list, np);
	of_node_put(np);
	if (!match)
		return -ENODEV;

	ret = nvmem_cell_read_u32(cpu_dev, "speed_grade", &cell_value);
	if (ret)
		return ret;

	speed_grade = (cell_value & OCOTP_CFG3_SPEED_GRADE_MASK) >> OCOTP_CFG3_SPEED_GRADE_SHIFT;
	mkt_segment = (cell_value & OCOTP_CFG3_MKT_SEGMENT_MASK) >> OCOTP_CFG3_MKT_SEGMENT_SHIFT;
	supported_hw[0] = BIT(speed_grade);
	supported_hw[1] = BIT(mkt_segment);
	dev_info(&pdev->dev, "cpu speed grade %d mkt segment %d supported-hw %#x %#x\n",
			speed_grade, mkt_segment, supported_hw[0], supported_hw[1]);

	cpufreq_opp_table = dev_pm_opp_set_supported_hw(cpu_dev, supported_hw, 2);
	if (IS_ERR(cpufreq_opp_table)) {
		ret = PTR_ERR(cpufreq_opp_table);
		dev_err(&pdev->dev, "Failed to set supported opp: %d\n", ret);
		return ret;
	}

	cpufreq_dt_pdev = platform_device_register_data(
			&pdev->dev, "cpufreq-dt", -1, NULL, 0);
	if (IS_ERR(cpufreq_dt_pdev)) {
		dev_pm_opp_put_supported_hw(cpufreq_opp_table);
		ret = PTR_ERR(cpufreq_dt_pdev);
		dev_err(&pdev->dev, "Failed to register cpufreq-dt: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx_cpufreq_dt_remove(struct platform_device *pdev)
{
	platform_device_unregister(cpufreq_dt_pdev);
	dev_pm_opp_put_supported_hw(cpufreq_opp_table);

	return 0;
}

static struct platform_driver imx_cpufreq_dt_driver = {
	.probe = imx_cpufreq_dt_probe,
	.remove = imx_cpufreq_dt_remove,
	.driver = {
		.name = "imx-cpufreq-dt",
	},
};
module_platform_driver(imx_cpufreq_dt_driver);

MODULE_ALIAS("platform:imx-cpufreq-dt");
MODULE_DESCRIPTION("Freescale i.MX cpufreq speed grading driver");
MODULE_LICENSE("GPL v2");
