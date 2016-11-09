/*
 * twl-common.c
 *
 * Copyright (C) 2011 Texas Instruments, Inc..
 * Author: Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/i2c.h>
#include <linux/i2c/twl.h>
#include <linux/gpio.h>
#include <linux/string.h>
#include <linux/phy/phy.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/fixed.h>

#include "soc.h"
#include "twl-common.h"
#include "pm.h"
#include "voltage.h"
#include "mux.h"

static struct i2c_board_info __initdata pmic_i2c_board_info = {
	.addr		= 0x48,
	.flags		= I2C_CLIENT_WAKE,
};

#if defined(CONFIG_ARCH_OMAP3)
static int twl_set_voltage(void *data, int target_uV)
{
	struct voltagedomain *voltdm = (struct voltagedomain *)data;
	return voltdm_scale(voltdm, target_uV);
}

static int twl_get_voltage(void *data)
{
	struct voltagedomain *voltdm = (struct voltagedomain *)data;
	return voltdm_get_voltage(voltdm);
}
#endif

void __init omap_pmic_init(int bus, u32 clkrate,
			   const char *pmic_type, int pmic_irq,
			   struct twl4030_platform_data *pmic_data)
{
	omap_mux_init_signal("sys_nirq", OMAP_PIN_INPUT_PULLUP | OMAP_PIN_OFF_WAKEUPENABLE);
	strlcpy(pmic_i2c_board_info.type, pmic_type,
		sizeof(pmic_i2c_board_info.type));
	pmic_i2c_board_info.irq = pmic_irq;
	pmic_i2c_board_info.platform_data = pmic_data;

	omap_register_i2c_bus(bus, clkrate, &pmic_i2c_board_info, 1);
}

void __init omap_pmic_late_init(void)
{
	/* Init the OMAP TWL parameters (if PMIC has been registerd) */
	if (!pmic_i2c_board_info.irq)
		return;

	omap3_twl_init();
	omap4_twl_init();
}

#if defined(CONFIG_ARCH_OMAP3)
static struct twl4030_usb_data omap3_usb_pdata = {
	.usb_mode = T2_USB_MODE_ULPI,
};

static int omap3_batt_table[] = {
/* 0 C */
30800, 29500, 28300, 27100,
26000, 24900, 23900, 22900, 22000, 21100, 20300, 19400, 18700, 17900,
17200, 16500, 15900, 15300, 14700, 14100, 13600, 13100, 12600, 12100,
11600, 11200, 10800, 10400, 10000, 9630,  9280,  8950,  8620,  8310,
8020,  7730,  7460,  7200,  6950,  6710,  6470,  6250,  6040,  5830,
5640,  5450,  5260,  5090,  4920,  4760,  4600,  4450,  4310,  4170,
4040,  3910,  3790,  3670,  3550
};

static struct twl4030_bci_platform_data omap3_bci_pdata = {
	.battery_tmp_tbl	= omap3_batt_table,
	.tblsize		= ARRAY_SIZE(omap3_batt_table),
};

static struct twl4030_madc_platform_data omap3_madc_pdata = {
	.irq_line	= 1,
};

static struct twl4030_codec_data omap3_codec;

static struct twl4030_audio_data omap3_audio_pdata = {
	.audio_mclk = 26000000,
	.codec = &omap3_codec,
};

static struct regulator_consumer_supply omap3_vdda_dac_supplies[] = {
	REGULATOR_SUPPLY("vdda_dac", "omapdss_venc"),
};

static struct regulator_init_data omap3_vdac_idata = {
	.constraints = {
		.min_uV			= 1800000,
		.max_uV			= 1800000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= ARRAY_SIZE(omap3_vdda_dac_supplies),
	.consumer_supplies	= omap3_vdda_dac_supplies,
};

static struct regulator_consumer_supply omap3_vpll2_supplies[] = {
	REGULATOR_SUPPLY("vdds_dsi", "omapdss"),
	REGULATOR_SUPPLY("vdds_dsi", "omapdss_dpi.0"),
	REGULATOR_SUPPLY("vdds_dsi", "omapdss_dsi.0"),
};

static struct regulator_init_data omap3_vpll2_idata = {
	.constraints = {
		.min_uV                 = 1800000,
		.max_uV                 = 1800000,
		.valid_modes_mask       = REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask         = REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies		= ARRAY_SIZE(omap3_vpll2_supplies),
	.consumer_supplies		= omap3_vpll2_supplies,
};

static struct regulator_consumer_supply omap3_vdd1_supply[] = {
	REGULATOR_SUPPLY("vcc", "cpu0"),
};

static struct regulator_consumer_supply omap3_vdd2_supply[] = {
	REGULATOR_SUPPLY("vcc", "l3_main.0"),
};

static struct regulator_init_data omap3_vdd1 = {
	.constraints = {
		.name			= "vdd_mpu_iva",
		.min_uV			= 600000,
		.max_uV			= 1450000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies		= ARRAY_SIZE(omap3_vdd1_supply),
	.consumer_supplies		= omap3_vdd1_supply,
};

static struct regulator_init_data omap3_vdd2 = {
	.constraints = {
		.name			= "vdd_core",
		.min_uV			= 600000,
		.max_uV			= 1450000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies		= ARRAY_SIZE(omap3_vdd2_supply),
	.consumer_supplies		= omap3_vdd2_supply,
};

static struct twl_regulator_driver_data omap3_vdd1_drvdata = {
	.get_voltage = twl_get_voltage,
	.set_voltage = twl_set_voltage,
};

static struct twl_regulator_driver_data omap3_vdd2_drvdata = {
	.get_voltage = twl_get_voltage,
	.set_voltage = twl_set_voltage,
};

void __init omap3_pmic_get_config(struct twl4030_platform_data *pmic_data,
				  u32 pdata_flags, u32 regulators_flags)
{
	if (!pmic_data->vdd1) {
		omap3_vdd1.driver_data = &omap3_vdd1_drvdata;
		omap3_vdd1_drvdata.data = voltdm_lookup("mpu_iva");
		pmic_data->vdd1 = &omap3_vdd1;
	}
	if (!pmic_data->vdd2) {
		omap3_vdd2.driver_data = &omap3_vdd2_drvdata;
		omap3_vdd2_drvdata.data = voltdm_lookup("core");
		pmic_data->vdd2 = &omap3_vdd2;
	}

	/* Common platform data configurations */
	if (pdata_flags & TWL_COMMON_PDATA_USB && !pmic_data->usb)
		pmic_data->usb = &omap3_usb_pdata;

	if (pdata_flags & TWL_COMMON_PDATA_BCI && !pmic_data->bci)
		pmic_data->bci = &omap3_bci_pdata;

	if (pdata_flags & TWL_COMMON_PDATA_MADC && !pmic_data->madc)
		pmic_data->madc = &omap3_madc_pdata;

	if (pdata_flags & TWL_COMMON_PDATA_AUDIO && !pmic_data->audio)
		pmic_data->audio = &omap3_audio_pdata;

	/* Common regulator configurations */
	if (regulators_flags & TWL_COMMON_REGULATOR_VDAC && !pmic_data->vdac)
		pmic_data->vdac = &omap3_vdac_idata;

	if (regulators_flags & TWL_COMMON_REGULATOR_VPLL2 && !pmic_data->vpll2)
		pmic_data->vpll2 = &omap3_vpll2_idata;
}
#endif /* CONFIG_ARCH_OMAP3 */

#if IS_ENABLED(CONFIG_SND_OMAP_SOC_OMAP_TWL4030)
#include <linux/platform_data/omap-twl4030.h>

/* Commonly used configuration */
static struct omap_tw4030_pdata omap_twl4030_audio_data;

static struct platform_device audio_device = {
	.name		= "omap-twl4030",
	.id		= -1,
};

void omap_twl4030_audio_init(char *card_name,
				    struct omap_tw4030_pdata *pdata)
{
	if (!pdata)
		pdata = &omap_twl4030_audio_data;

	pdata->card_name = card_name;

	audio_device.dev.platform_data = pdata;
	platform_device_register(&audio_device);
}

#else /* SOC_OMAP_TWL4030 */
void omap_twl4030_audio_init(char *card_name,
				    struct omap_tw4030_pdata *pdata)
{
	return;
}
#endif /* SOC_OMAP_TWL4030 */
