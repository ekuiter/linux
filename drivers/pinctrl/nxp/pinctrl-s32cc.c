// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Core driver for the S32 CC (Common Chassis) pin controller
 *
 * Copyright 2017-2022 NXP
 * Copyright (C) 2022 SUSE LLC
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"
#include "pinctrl-s32.h"

#define S32_PIN_ID_MASK		GENMASK(31, 4)

#define S32_MSCR_SSS_MASK	GENMASK(2, 0)
#define S32_MSCR_PUS		BIT(12)
#define S32_MSCR_PUE		BIT(13)
#define S32_MSCR_SRE(X)		(((X) & GENMASK(3, 0)) << 14)
#define S32_MSCR_IBE		BIT(19)
#define S32_MSCR_ODE		BIT(20)
#define S32_MSCR_OBE		BIT(21)

static struct regmap_config s32_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static u32 get_pin_no(u32 pinmux)
{
	return (pinmux & S32_PIN_ID_MASK) >> __ffs(S32_PIN_ID_MASK);
}

static u32 get_pin_func(u32 pinmux)
{
	return pinmux & GENMASK(3, 0);
}

struct s32_pinctrl_mem_region {
	struct regmap *map;
	const struct s32_pin_range *pin_range;
	char name[8];
};

/*
 * Holds pin configuration for GPIO's.
 * @pin_id: Pin ID for this GPIO
 * @config: Pin settings
 * @list: Linked list entry for each gpio pin
 */
struct gpio_pin_config {
	unsigned int pin_id;
	unsigned int config;
	struct list_head list;
};

/*
 * Pad config save/restore for power suspend/resume.
 */
struct s32_pinctrl_context {
	unsigned int *pads;
};

/*
 * @dev: a pointer back to containing device
 * @pctl: a pointer to the pinctrl device structure
 * @regions: reserved memory regions with start/end pin
 * @info: structure containing information about the pin
 * @gpio_configs: Saved configurations for GPIO pins
 * @gpiop_configs_lock: lock for the `gpio_configs` list
 * @s32_pinctrl_context: Configuration saved over system sleep
 */
struct s32_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl;
	struct s32_pinctrl_mem_region *regions;
	struct s32_pinctrl_soc_info *info;
	struct list_head gpio_configs;
	spinlock_t gpio_configs_lock;
#ifdef CONFIG_PM_SLEEP
	struct s32_pinctrl_context saved_context;
#endif
};

static struct s32_pinctrl_mem_region *
s32_get_region(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pin_range *pin_range;
	unsigned int mem_regions = ipctl->info->mem_regions;
	unsigned int i;

	for (i = 0; i < mem_regions; ++i) {
		pin_range = ipctl->regions[i].pin_range;
		if (pin >= pin_range->start && pin <= pin_range->end)
			return &ipctl->regions[i];
	}

	return NULL;
}

static inline int s32_check_pin(struct pinctrl_dev *pctldev,
				unsigned int pin)
{
	return s32_get_region(pctldev, pin) ? 0 : -EINVAL;
}

static inline int s32_regmap_read(struct pinctrl_dev *pctldev,
			   unsigned int pin, unsigned int *val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_read(region->map, offset, val);
}

static inline int s32_regmap_write(struct pinctrl_dev *pctldev,
			    unsigned int pin,
			    unsigned int val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_write(region->map, offset, val);

}

static inline int s32_regmap_update(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned int mask, unsigned int val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_update_bits(region->map, offset, mask, val);
}

static int s32_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->ngroups;
}

static const char *s32_get_group_name(struct pinctrl_dev *pctldev,
				      unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->groups[selector].name;
}

static int s32_get_group_pins(struct pinctrl_dev *pctldev,
			      unsigned int selector, const unsigned int **pins,
			      unsigned int *npins)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	*pins = info->groups[selector].pin_ids;
	*npins = info->groups[selector].npins;

	return 0;
}

static void s32_pin_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
			     unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static int s32_dt_group_node_to_map(struct pinctrl_dev *pctldev,
				    struct device_node *np,
				    struct pinctrl_map **map,
				    unsigned int *reserved_maps,
				    unsigned int *num_maps,
				    const char *func_name)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct device *dev = ipctl->dev;
	unsigned long *cfgs = NULL;
	unsigned int n_cfgs, reserve = 1;
	int n_pins, ret;

	n_pins = of_property_count_elems_of_size(np, "pinmux", sizeof(u32));
	if (n_pins < 0) {
		dev_warn(dev, "Unable to find 'pinmux' property in node %s.\n",
			np->name);
	} else if (!n_pins) {
		return -EINVAL;
	}

	ret = pinconf_generic_parse_dt_config(np, pctldev, &cfgs, &n_cfgs);
	if (ret) {
		dev_err(dev, "%pOF: could not parse node property\n", np);
		return ret;
	}

	if (n_cfgs)
		reserve++;

	ret = pinctrl_utils_reserve_map(pctldev, map, reserved_maps, num_maps,
					reserve);
	if (ret < 0)
		goto free_cfgs;

	ret = pinctrl_utils_add_map_mux(pctldev, map, reserved_maps, num_maps,
					np->name, func_name);
	if (ret < 0)
		goto free_cfgs;

	if (n_cfgs) {
		ret = pinctrl_utils_add_map_configs(pctldev, map, reserved_maps,
						    num_maps, np->name, cfgs, n_cfgs,
						    PIN_MAP_TYPE_CONFIGS_GROUP);
		if (ret < 0)
			goto free_cfgs;
	}

free_cfgs:
	kfree(cfgs);
	return ret;
}

static int s32_dt_node_to_map(struct pinctrl_dev *pctldev,
			      struct device_node *np_config,
			      struct pinctrl_map **map,
			      unsigned int *num_maps)
{
	unsigned int reserved_maps;
	struct device_node *np;
	int ret = 0;

	reserved_maps = 0;
	*map = NULL;
	*num_maps = 0;

	for_each_available_child_of_node(np_config, np) {
		ret = s32_dt_group_node_to_map(pctldev, np, map,
					       &reserved_maps, num_maps,
					       np_config->name);
		if (ret < 0)
			break;
	}

	if (ret)
		pinctrl_utils_free_map(pctldev, *map, *num_maps);

	return ret;

}

static const struct pinctrl_ops s32_pctrl_ops = {
	.get_groups_count = s32_get_groups_count,
	.get_group_name = s32_get_group_name,
	.get_group_pins = s32_get_group_pins,
	.pin_dbg_show = s32_pin_dbg_show,
	.dt_node_to_map = s32_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

static int s32_pmx_set(struct pinctrl_dev *pctldev, unsigned int selector,
		       unsigned int group)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	int i, ret;
	struct s32_pin_group *grp;

	/*
	 * Configure the mux mode for each pin in the group for a specific
	 * function.
	 */
	grp = &info->groups[group];

	dev_dbg(ipctl->dev, "set mux for function %s group %s\n",
		info->functions[selector].name, grp->name);

	/* Check beforehand so we don't have a partial config. */
	for (i = 0; i < grp->npins; ++i) {
		if (s32_check_pin(pctldev, grp->pin_ids[i]) != 0) {
			dev_err(info->dev, "invalid pin: %d in group: %d\n",
				grp->pin_ids[i], group);
			return -EINVAL;
		}
	}

	for (i = 0, ret = 0; i < grp->npins && !ret; ++i) {
		ret = s32_regmap_update(pctldev, grp->pin_ids[i],
					S32_MSCR_SSS_MASK, grp->pin_sss[i]);
	}

	return ret;
}

static int s32_pmx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->nfunctions;
}

static const char *s32_pmx_get_func_name(struct pinctrl_dev *pctldev,
					 unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->functions[selector].name;
}

static int s32_pmx_get_groups(struct pinctrl_dev *pctldev,
			      unsigned int selector,
			      const char * const **groups,
			      unsigned int * const num_groups)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	*groups = info->functions[selector].groups;
	*num_groups = info->functions[selector].num_groups;

	return 0;
}

static int s32_pmx_gpio_request_enable(struct pinctrl_dev *pctldev,
				       struct pinctrl_gpio_range *range,
				       unsigned int offset)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct gpio_pin_config *gpio_pin;
	unsigned int config;
	unsigned long flags;
	int ret;

	ret = s32_regmap_read(pctldev, offset, &config);
	if (ret != 0)
		return -EINVAL;

	/* Save current configuration */
	gpio_pin = kmalloc(sizeof(*gpio_pin), GFP_KERNEL);
	if (!gpio_pin)
		return -ENOMEM;

	gpio_pin->pin_id = offset;
	gpio_pin->config = config;

	spin_lock_irqsave(&ipctl->gpio_configs_lock, flags);
	list_add(&(gpio_pin->list), &(ipctl->gpio_configs));
	spin_unlock_irqrestore(&ipctl->gpio_configs_lock, flags);

	/* GPIO pin means SSS = 0 */
	config &= ~S32_MSCR_SSS_MASK;

	return s32_regmap_write(pctldev, offset, config);
}

static void s32_pmx_gpio_disable_free(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int offset)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct list_head *pos, *tmp;
	struct gpio_pin_config *gpio_pin;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ipctl->gpio_configs_lock, flags);

	list_for_each_safe(pos, tmp, &ipctl->gpio_configs) {
		gpio_pin = list_entry(pos, struct gpio_pin_config, list);

		if (gpio_pin->pin_id == offset) {
			ret = s32_regmap_write(pctldev, gpio_pin->pin_id,
						 gpio_pin->config);
			if (ret != 0)
				goto unlock;

			list_del(pos);
			kfree(gpio_pin);
			break;
		}
	}

unlock:
	spin_unlock_irqrestore(&ipctl->gpio_configs_lock, flags);
}

static int s32_pmx_gpio_set_direction(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int offset,
				      bool input)
{
	unsigned int config;
	unsigned int mask = S32_MSCR_IBE | S32_MSCR_OBE;

	if (input) {
		/* Disable output buffer and enable input buffer */
		config = S32_MSCR_IBE;
	} else {
		/* Disable input buffer and enable output buffer */
		config = S32_MSCR_OBE;
	}

	return s32_regmap_update(pctldev, offset, mask, config);
}

static const struct pinmux_ops s32_pmx_ops = {
	.get_functions_count = s32_pmx_get_funcs_count,
	.get_function_name = s32_pmx_get_func_name,
	.get_function_groups = s32_pmx_get_groups,
	.set_mux = s32_pmx_set,
	.gpio_request_enable = s32_pmx_gpio_request_enable,
	.gpio_disable_free = s32_pmx_gpio_disable_free,
	.gpio_set_direction = s32_pmx_gpio_set_direction,
};

/* Set the reserved elements as -1 */
static const int support_slew[] = {208, -1, -1, -1, 166, 150, 133, 83};

static int s32_get_slew_regval(int arg)
{
	int i;
	/* Translate a real slew rate (MHz) to a register value */
	for (i = 0; i < ARRAY_SIZE(support_slew); i++) {
		if (arg == support_slew[i])
			return i;
	}

	return -EINVAL;
}

static int s32_get_pin_conf(enum pin_config_param param, u32 arg,
			    unsigned int *mask, unsigned int *config)
{
	int ret;

	switch (param) {
	/* All pins are persistent over suspend */
	case PIN_CONFIG_PERSIST_STATE:
		return 0;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		*config |= S32_MSCR_ODE;
		*mask |= S32_MSCR_ODE;
		break;
	case PIN_CONFIG_OUTPUT_ENABLE:
		if (arg)
			*config |= S32_MSCR_OBE;
		else
			*config &= ~S32_MSCR_OBE;
		*mask |= S32_MSCR_OBE;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		if (arg)
			*config |= S32_MSCR_IBE;
		else
			*config &= ~S32_MSCR_IBE;
		*mask |= S32_MSCR_IBE;
		break;
	case PIN_CONFIG_SLEW_RATE:
		ret = s32_get_slew_regval(arg);
		if (ret < 0)
			return ret;
		*config |= S32_MSCR_SRE((u32)ret);
		*mask |= S32_MSCR_SRE(~0);
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (arg)
			*config |= S32_MSCR_PUS;
		else
			*config &= ~S32_MSCR_PUS;
		fallthrough;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (arg)
			*config |= S32_MSCR_PUE;
		else
			*config &= ~S32_MSCR_PUE;
		*mask |= S32_MSCR_PUE | S32_MSCR_PUS;
		break;
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		*config &= ~(S32_MSCR_ODE | S32_MSCR_OBE | S32_MSCR_IBE);
		*mask |= S32_MSCR_ODE | S32_MSCR_OBE | S32_MSCR_IBE;
		fallthrough;
	case PIN_CONFIG_BIAS_DISABLE:
		*config &= ~(S32_MSCR_PUS | S32_MSCR_PUE);
		*mask |= S32_MSCR_PUS | S32_MSCR_PUE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int s32_pinconf_mscr_update(struct pinctrl_dev *pctldev,
				   unsigned int pin_id,
				   unsigned long *configs,
				   unsigned int num_configs)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int config = 0, mask = 0;
	int i, ret;

	if (s32_check_pin(pctldev, pin_id) != 0)
		return -EINVAL;

	dev_dbg(ipctl->dev, "pinconf set pin %s with %d configs\n",
		pin_get_name(pctldev, pin_id), num_configs);

	for (i = 0; i < num_configs; i++) {
		ret = s32_get_pin_conf(pinconf_to_config_param(configs[i]),
				       pinconf_to_config_argument(configs[i]),
				       &mask, &config);
		if (ret)
			return ret;
	}

	if (!config && !mask)
		return 0;

	ret = s32_regmap_update(pctldev, pin_id, mask, config);

	dev_dbg(ipctl->dev, "update: pin %d cfg 0x%x\n", pin_id, config);

	return ret;
}

static int s32_pinconf_get(struct pinctrl_dev *pctldev,
			   unsigned int pin_id,
			   unsigned long *config)
{
	return s32_regmap_read(pctldev, pin_id, (unsigned int *)config);
}

static int s32_pinconf_set(struct pinctrl_dev *pctldev,
			   unsigned int pin_id, unsigned long *configs,
			   unsigned int num_configs)
{
	return s32_pinconf_mscr_update(pctldev, pin_id, configs,
				       num_configs);
}

static int s32_pconf_group_set(struct pinctrl_dev *pctldev, unsigned int selector,
			       unsigned long *configs, unsigned int num_configs)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pin_group *grp;
	int i, ret;

	grp = &info->groups[selector];
	for (i = 0; i < grp->npins; i++) {
		ret = s32_pinconf_mscr_update(pctldev, grp->pin_ids[i],
					      configs, num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

static void s32_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				 struct seq_file *s, unsigned int pin_id)
{
	unsigned int config;
	int ret = s32_regmap_read(pctldev, pin_id, &config);

	if (!ret)
		seq_printf(s, "0x%x", config);
}

static void s32_pinconf_group_dbg_show(struct pinctrl_dev *pctldev,
				       struct seq_file *s, unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pin_group *grp;
	unsigned int config;
	const char *name;
	int i, ret;

	seq_puts(s, "\n");
	grp = &info->groups[selector];
	for (i = 0; i < grp->npins; i++) {
		name = pin_get_name(pctldev, grp->pin_ids[i]);
		ret = s32_regmap_read(pctldev, grp->pin_ids[i], &config);
		if (ret)
			return;
		seq_printf(s, "%s: 0x%x\n", name, config);
	}
}

static const struct pinconf_ops s32_pinconf_ops = {
	.pin_config_get = s32_pinconf_get,
	.pin_config_set	= s32_pinconf_set,
	.pin_config_group_set = s32_pconf_group_set,
	.pin_config_dbg_show = s32_pinconf_dbg_show,
	.pin_config_group_dbg_show = s32_pinconf_group_dbg_show,
};

#ifdef CONFIG_PM_SLEEP
static bool s32_pinctrl_should_save(struct s32_pinctrl *ipctl,
				    unsigned int pin)
{
	const struct pin_desc *pd = pin_desc_get(ipctl->pctl, pin);

	if (!pd)
		return false;

	/*
	 * Only restore the pin if it is actually in use by the kernel (or
	 * by userspace).
	 */
	if (pd->mux_owner || pd->gpio_owner)
		return true;

	return false;
}

int s32_pinctrl_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s32_pinctrl *ipctl = platform_get_drvdata(pdev);
	const struct pinctrl_pin_desc *pin;
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pinctrl_context *saved_context = &ipctl->saved_context;
	int i;
	int ret;
	unsigned int config;

	for (i = 0; i < info->npins; i++) {
		pin = &info->pins[i];

		if (!s32_pinctrl_should_save(ipctl, pin->number))
			continue;

		ret = s32_regmap_read(ipctl->pctl, pin->number, &config);
		if (ret)
			return -EINVAL;

		saved_context->pads[i] = config;
	}

	return 0;
}

int s32_pinctrl_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s32_pinctrl *ipctl = platform_get_drvdata(pdev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	const struct pinctrl_pin_desc *pin;
	struct s32_pinctrl_context *saved_context = &ipctl->saved_context;
	int ret, i;

	for (i = 0; i < info->npins; i++) {
		pin = &info->pins[i];

		if (!s32_pinctrl_should_save(ipctl, pin->number))
			continue;

		ret = s32_regmap_write(ipctl->pctl, pin->number,
					 saved_context->pads[i]);
		if (ret)
			return ret;
	}

	return 0;
}
#endif

static void s32_pinctrl_parse_groups(struct device_node *np,
				     struct s32_pin_group *grp,
				     struct s32_pinctrl_soc_info *info)
{
	const __be32 *p;
	struct device *dev;
	struct property *prop;
	int i, npins;
	u32 pinmux;

	dev = info->dev;

	dev_dbg(dev, "group: %s\n", np->name);

	/* Initialise group */
	grp->name = np->name;

	npins = of_property_count_elems_of_size(np, "pinmux", sizeof(u32));

	if (npins < 0) {
		dev_err(dev, "Failed to read 'pinmux' property in node %s.\n",
			np->name);
		return;
	}
	if (!npins) {
		dev_err(dev, "The group %s has no pins.\n", np->name);
		return;
	}

	grp->npins = npins;

	grp->pin_ids = devm_kcalloc(info->dev, grp->npins,
				    sizeof(unsigned int), GFP_KERNEL);
	grp->pin_sss = devm_kcalloc(info->dev, grp->npins,
				    sizeof(unsigned int), GFP_KERNEL);

	if (!grp->pin_ids || !grp->pin_sss) {
		dev_err(dev, "Failed to allocate memory for the group %s.\n",
			np->name);
		return;
	}

	i = 0;
	of_property_for_each_u32(np, "pinmux", prop, p, pinmux) {
		grp->pin_ids[i] = get_pin_no(pinmux);
		grp->pin_sss[i] = get_pin_func(pinmux);

		dev_dbg(info->dev, "pin-id: 0x%x, sss: 0x%x",
			grp->pin_ids[i], grp->pin_sss[i]);
		i++;
	}
}

static void s32_pinctrl_parse_functions(struct device_node *np,
					struct s32_pinctrl_soc_info *info,
					u32 index)
{
	struct device_node *child;
	struct s32_pmx_func *func;
	struct s32_pin_group *grp;
	u32 i = 0;

	dev_dbg(info->dev, "parse function(%d): %s\n", index, np->name);

	func = &info->functions[index];

	/* Initialise function */
	func->name = np->name;
	func->num_groups = of_get_child_count(np);
	if (func->num_groups == 0) {
		dev_err(info->dev, "no groups defined in %s\n", np->full_name);
		return;
	}
	func->groups = devm_kzalloc(info->dev,
			func->num_groups * sizeof(char *), GFP_KERNEL);

	for_each_child_of_node(np, child) {
		func->groups[i] = child->name;
		grp = &info->groups[info->grp_index++];
		s32_pinctrl_parse_groups(child, grp, info);
		i++;
	}
}

static int s32_pinctrl_probe_dt(struct platform_device *pdev,
				struct s32_pinctrl *ipctl)
{
	struct s32_pinctrl_soc_info *info = ipctl->info;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct resource *res;
	struct regmap *map;
	void __iomem *base;
	int mem_regions = info->mem_regions;
	u32 nfuncs = 0;
	u32 i = 0;

	if (!np)
		return -ENODEV;

	if (mem_regions == 0) {
		dev_err(&pdev->dev, "mem_regions is 0\n");
		return -EINVAL;
	}

	ipctl->regions = devm_kzalloc(&pdev->dev,
				      mem_regions * sizeof(*(ipctl->regions)),
				      GFP_KERNEL);
	if (!ipctl->regions)
		return -ENOMEM;

	for (i = 0; i < mem_regions; ++i) {
		base = devm_platform_get_and_ioremap_resource(pdev, i, &res);
		if (IS_ERR(base))
			return PTR_ERR(base);

		snprintf(ipctl->regions[i].name,
			 sizeof(ipctl->regions[i].name), "map%u", i);

		s32_regmap_config.name = ipctl->regions[i].name;
		s32_regmap_config.max_register = resource_size(res) -
						 s32_regmap_config.reg_stride;

		map = devm_regmap_init_mmio(&pdev->dev, base,
						&s32_regmap_config);
		if (IS_ERR(map)) {
			dev_err(&pdev->dev, "Failed to init regmap[%u]\n", i);
			return PTR_ERR(map);
		}

		ipctl->regions[i].map = map;
		ipctl->regions[i].pin_range = &info->mem_pin_ranges[i];
	}

	nfuncs = of_get_child_count(np);
	if (nfuncs <= 0) {
		dev_err(&pdev->dev, "no functions defined\n");
		return -EINVAL;
	}

	info->nfunctions = nfuncs;
	info->functions = devm_kzalloc(&pdev->dev,
				       nfuncs * sizeof(struct s32_pmx_func),
				       GFP_KERNEL);
	if (!info->functions)
		return -ENOMEM;

	info->ngroups = 0;
	for_each_child_of_node(np, child)
		info->ngroups += of_get_child_count(child);
	info->groups = devm_kzalloc(&pdev->dev,
				    info->ngroups * sizeof(struct s32_pin_group),
				    GFP_KERNEL);
	if (!info->groups)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node(np, child)
		s32_pinctrl_parse_functions(child, info, i++);

	return 0;
}

int s32_pinctrl_probe(struct platform_device *pdev,
		      struct s32_pinctrl_soc_info *info)
{
	struct s32_pinctrl *ipctl;
	int ret;
	struct pinctrl_desc *s32_pinctrl_desc;
#ifdef CONFIG_PM_SLEEP
	struct s32_pinctrl_context *saved_context;
#endif

	if (!info || !info->pins || !info->npins) {
		dev_err(&pdev->dev, "wrong pinctrl info\n");
		return -EINVAL;
	}

	info->dev = &pdev->dev;

	/* Create state holders etc for this driver */
	ipctl = devm_kzalloc(&pdev->dev, sizeof(*ipctl), GFP_KERNEL);
	if (!ipctl)
		return -ENOMEM;

	ipctl->info = info;
	ipctl->dev = info->dev;
	platform_set_drvdata(pdev, ipctl);

	INIT_LIST_HEAD(&ipctl->gpio_configs);
	spin_lock_init(&ipctl->gpio_configs_lock);

	s32_pinctrl_desc =
		devm_kmalloc(&pdev->dev, sizeof(*s32_pinctrl_desc), GFP_KERNEL);
	if (!s32_pinctrl_desc)
		return -ENOMEM;

	s32_pinctrl_desc->name = dev_name(&pdev->dev);
	s32_pinctrl_desc->pins = info->pins;
	s32_pinctrl_desc->npins = info->npins;
	s32_pinctrl_desc->pctlops = &s32_pctrl_ops;
	s32_pinctrl_desc->pmxops = &s32_pmx_ops;
	s32_pinctrl_desc->confops = &s32_pinconf_ops;
	s32_pinctrl_desc->owner = THIS_MODULE;

	ret = s32_pinctrl_probe_dt(pdev, ipctl);
	if (ret) {
		dev_err(&pdev->dev, "fail to probe dt properties\n");
		return ret;
	}

	ipctl->pctl = devm_pinctrl_register(&pdev->dev, s32_pinctrl_desc,
					    ipctl);

	if (IS_ERR(ipctl->pctl)) {
		dev_err(&pdev->dev, "could not register s32 pinctrl driver\n");
		return PTR_ERR(ipctl->pctl);
	}

#ifdef CONFIG_PM_SLEEP
	saved_context = &ipctl->saved_context;
	saved_context->pads =
		devm_kcalloc(&pdev->dev, info->npins,
			     sizeof(*saved_context->pads),
			     GFP_KERNEL);
	if (!saved_context->pads)
		return -ENOMEM;
#endif

	dev_info(&pdev->dev, "initialized s32 pinctrl driver\n");

	return 0;
}
