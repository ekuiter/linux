/*
 * Copyright (c) 2012, 2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/io.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/clk/tegra.h>
#include <dt-bindings/clock/tegra114-car.h>

#include "clk.h"

#define RST_DFLL_DVCO			0x2F4
#define CPU_FINETRIM_SELECT		0x4d4	/* override default prop dlys */
#define CPU_FINETRIM_DR			0x4d8	/* rise->rise prop dly A */
#define CPU_FINETRIM_R			0x4e4	/* rise->rise prop dly inc A */

/* RST_DFLL_DVCO bitfields */
#define DVFS_DFLL_RESET_SHIFT		0

/* CPU_FINETRIM_SELECT and CPU_FINETRIM_DR bitfields */
#define CPU_FINETRIM_1_FCPU_1		BIT(0)	/* fcpu0 */
#define CPU_FINETRIM_1_FCPU_2		BIT(1)	/* fcpu1 */
#define CPU_FINETRIM_1_FCPU_3		BIT(2)	/* fcpu2 */
#define CPU_FINETRIM_1_FCPU_4		BIT(3)	/* fcpu3 */
#define CPU_FINETRIM_1_FCPU_5		BIT(4)	/* fl2 */
#define CPU_FINETRIM_1_FCPU_6		BIT(5)	/* ftop */

/* CPU_FINETRIM_R bitfields */
#define CPU_FINETRIM_R_FCPU_1_SHIFT	0		/* fcpu0 */
#define CPU_FINETRIM_R_FCPU_1_MASK	(0x3 << CPU_FINETRIM_R_FCPU_1_SHIFT)
#define CPU_FINETRIM_R_FCPU_2_SHIFT	2		/* fcpu1 */
#define CPU_FINETRIM_R_FCPU_2_MASK	(0x3 << CPU_FINETRIM_R_FCPU_2_SHIFT)
#define CPU_FINETRIM_R_FCPU_3_SHIFT	4		/* fcpu2 */
#define CPU_FINETRIM_R_FCPU_3_MASK	(0x3 << CPU_FINETRIM_R_FCPU_3_SHIFT)
#define CPU_FINETRIM_R_FCPU_4_SHIFT	6		/* fcpu3 */
#define CPU_FINETRIM_R_FCPU_4_MASK	(0x3 << CPU_FINETRIM_R_FCPU_4_SHIFT)
#define CPU_FINETRIM_R_FCPU_5_SHIFT	8		/* fl2 */
#define CPU_FINETRIM_R_FCPU_5_MASK	(0x3 << CPU_FINETRIM_R_FCPU_5_SHIFT)
#define CPU_FINETRIM_R_FCPU_6_SHIFT	10		/* ftop */
#define CPU_FINETRIM_R_FCPU_6_MASK	(0x3 << CPU_FINETRIM_R_FCPU_6_SHIFT)

#define TEGRA114_CLK_PERIPH_BANKS	5

#define PLLC_BASE 0x80
#define PLLC_MISC2 0x88
#define PLLC_MISC 0x8c
#define PLLC2_BASE 0x4e8
#define PLLC2_MISC 0x4ec
#define PLLC3_BASE 0x4fc
#define PLLC3_MISC 0x500
#define PLLM_BASE 0x90
#define PLLM_MISC 0x9c
#define PLLP_BASE 0xa0
#define PLLP_MISC 0xac
#define PLLX_BASE 0xe0
#define PLLX_MISC 0xe4
#define PLLX_MISC2 0x514
#define PLLX_MISC3 0x518
#define PLLD_BASE 0xd0
#define PLLD_MISC 0xdc
#define PLLD2_BASE 0x4b8
#define PLLD2_MISC 0x4bc
#define PLLE_BASE 0xe8
#define PLLE_MISC 0xec
#define PLLA_BASE 0xb0
#define PLLA_MISC 0xbc
#define PLLU_BASE 0xc0
#define PLLU_MISC 0xcc
#define PLLRE_BASE 0x4c4
#define PLLRE_MISC 0x4c8

#define PLL_MISC_LOCK_ENABLE 18
#define PLLC_MISC_LOCK_ENABLE 24
#define PLLDU_MISC_LOCK_ENABLE 22
#define PLLE_MISC_LOCK_ENABLE 9
#define PLLRE_MISC_LOCK_ENABLE 30

#define PLLC_IDDQ_BIT 26
#define PLLX_IDDQ_BIT 3
#define PLLRE_IDDQ_BIT 16

#define PLL_BASE_LOCK BIT(27)
#define PLLE_MISC_LOCK BIT(11)
#define PLLRE_MISC_LOCK BIT(24)
#define PLLCX_BASE_LOCK (BIT(26)|BIT(27))

#define PLLE_AUX 0x48c
#define PLLC_OUT 0x84
#define PLLM_OUT 0x94
#define PLLP_OUTA 0xa4
#define PLLP_OUTB 0xa8
#define PLLA_OUT 0xb4

#define AUDIO_SYNC_CLK_I2S0 0x4a0
#define AUDIO_SYNC_CLK_I2S1 0x4a4
#define AUDIO_SYNC_CLK_I2S2 0x4a8
#define AUDIO_SYNC_CLK_I2S3 0x4ac
#define AUDIO_SYNC_CLK_I2S4 0x4b0
#define AUDIO_SYNC_CLK_SPDIF 0x4b4

#define AUDIO_SYNC_DOUBLER 0x49c

#define PMC_CLK_OUT_CNTRL 0x1a8
#define PMC_DPD_PADS_ORIDE 0x1c
#define PMC_DPD_PADS_ORIDE_BLINK_ENB 20
#define PMC_CTRL 0
#define PMC_CTRL_BLINK_ENB 7
#define PMC_BLINK_TIMER 0x40

#define OSC_CTRL			0x50
#define OSC_CTRL_OSC_FREQ_SHIFT		28
#define OSC_CTRL_PLL_REF_DIV_SHIFT	26

#define PLLXC_SW_MAX_P			6

#define CCLKG_BURST_POLICY 0x368
#define CCLKLP_BURST_POLICY 0x370
#define SCLK_BURST_POLICY 0x028
#define SYSTEM_CLK_RATE 0x030

#define UTMIP_PLL_CFG2 0x488
#define UTMIP_PLL_CFG2_STABLE_COUNT(x) (((x) & 0xffff) << 6)
#define UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(x) (((x) & 0x3f) << 18)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN BIT(0)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN BIT(2)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN BIT(4)

#define UTMIP_PLL_CFG1 0x484
#define UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(x) (((x) & 0x1f) << 6)
#define UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(x) (((x) & 0xfff) << 0)
#define UTMIP_PLL_CFG1_FORCE_PLLU_POWERUP BIT(17)
#define UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN BIT(16)
#define UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERUP BIT(15)
#define UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN BIT(14)
#define UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN BIT(12)

#define UTMIPLL_HW_PWRDN_CFG0			0x52c
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_START_STATE	BIT(25)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_ENABLE	BIT(24)
#define UTMIPLL_HW_PWRDN_CFG0_USE_LOCKDET	BIT(6)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_RESET_INPUT_VALUE	BIT(5)
#define UTMIPLL_HW_PWRDN_CFG0_SEQ_IN_SWCTL	BIT(4)
#define UTMIPLL_HW_PWRDN_CFG0_CLK_ENABLE_SWCTL	BIT(2)
#define UTMIPLL_HW_PWRDN_CFG0_IDDQ_OVERRIDE	BIT(1)
#define UTMIPLL_HW_PWRDN_CFG0_IDDQ_SWCTL	BIT(0)

#define CLK_SOURCE_I2S0 0x1d8
#define CLK_SOURCE_I2S1 0x100
#define CLK_SOURCE_I2S2 0x104
#define CLK_SOURCE_NDFLASH 0x160
#define CLK_SOURCE_I2S3 0x3bc
#define CLK_SOURCE_I2S4 0x3c0
#define CLK_SOURCE_SPDIF_OUT 0x108
#define CLK_SOURCE_SPDIF_IN 0x10c
#define CLK_SOURCE_PWM 0x110
#define CLK_SOURCE_ADX 0x638
#define CLK_SOURCE_AMX 0x63c
#define CLK_SOURCE_HDA 0x428
#define CLK_SOURCE_HDA2CODEC_2X 0x3e4
#define CLK_SOURCE_SBC1 0x134
#define CLK_SOURCE_SBC2 0x118
#define CLK_SOURCE_SBC3 0x11c
#define CLK_SOURCE_SBC4 0x1b4
#define CLK_SOURCE_SBC5 0x3c8
#define CLK_SOURCE_SBC6 0x3cc
#define CLK_SOURCE_SATA_OOB 0x420
#define CLK_SOURCE_SATA 0x424
#define CLK_SOURCE_NDSPEED 0x3f8
#define CLK_SOURCE_VFIR 0x168
#define CLK_SOURCE_SDMMC1 0x150
#define CLK_SOURCE_SDMMC2 0x154
#define CLK_SOURCE_SDMMC3 0x1bc
#define CLK_SOURCE_SDMMC4 0x164
#define CLK_SOURCE_VDE 0x1c8
#define CLK_SOURCE_CSITE 0x1d4
#define CLK_SOURCE_LA 0x1f8
#define CLK_SOURCE_TRACE 0x634
#define CLK_SOURCE_OWR 0x1cc
#define CLK_SOURCE_NOR 0x1d0
#define CLK_SOURCE_MIPI 0x174
#define CLK_SOURCE_I2C1 0x124
#define CLK_SOURCE_I2C2 0x198
#define CLK_SOURCE_I2C3 0x1b8
#define CLK_SOURCE_I2C4 0x3c4
#define CLK_SOURCE_I2C5 0x128
#define CLK_SOURCE_UARTA 0x178
#define CLK_SOURCE_UARTB 0x17c
#define CLK_SOURCE_UARTC 0x1a0
#define CLK_SOURCE_UARTD 0x1c0
#define CLK_SOURCE_UARTE 0x1c4
#define CLK_SOURCE_UARTA_DBG 0x178
#define CLK_SOURCE_UARTB_DBG 0x17c
#define CLK_SOURCE_UARTC_DBG 0x1a0
#define CLK_SOURCE_UARTD_DBG 0x1c0
#define CLK_SOURCE_UARTE_DBG 0x1c4
#define CLK_SOURCE_3D 0x158
#define CLK_SOURCE_2D 0x15c
#define CLK_SOURCE_VI_SENSOR 0x1a8
#define CLK_SOURCE_VI 0x148
#define CLK_SOURCE_EPP 0x16c
#define CLK_SOURCE_MSENC 0x1f0
#define CLK_SOURCE_TSEC 0x1f4
#define CLK_SOURCE_HOST1X 0x180
#define CLK_SOURCE_HDMI 0x18c
#define CLK_SOURCE_DISP1 0x138
#define CLK_SOURCE_DISP2 0x13c
#define CLK_SOURCE_CILAB 0x614
#define CLK_SOURCE_CILCD 0x618
#define CLK_SOURCE_CILE 0x61c
#define CLK_SOURCE_DSIALP 0x620
#define CLK_SOURCE_DSIBLP 0x624
#define CLK_SOURCE_TSENSOR 0x3b8
#define CLK_SOURCE_D_AUDIO 0x3d0
#define CLK_SOURCE_DAM0 0x3d8
#define CLK_SOURCE_DAM1 0x3dc
#define CLK_SOURCE_DAM2 0x3e0
#define CLK_SOURCE_ACTMON 0x3e8
#define CLK_SOURCE_EXTERN1 0x3ec
#define CLK_SOURCE_EXTERN2 0x3f0
#define CLK_SOURCE_EXTERN3 0x3f4
#define CLK_SOURCE_I2CSLOW 0x3fc
#define CLK_SOURCE_SE 0x42c
#define CLK_SOURCE_MSELECT 0x3b4
#define CLK_SOURCE_DFLL_REF 0x62c
#define CLK_SOURCE_DFLL_SOC 0x630
#define CLK_SOURCE_SOC_THERM 0x644
#define CLK_SOURCE_XUSB_HOST_SRC 0x600
#define CLK_SOURCE_XUSB_FALCON_SRC 0x604
#define CLK_SOURCE_XUSB_FS_SRC 0x608
#define CLK_SOURCE_XUSB_SS_SRC 0x610
#define CLK_SOURCE_XUSB_DEV_SRC 0x60c
#define CLK_SOURCE_EMC 0x19c

/* PLLM override registers */
#define PMC_PLLM_WB0_OVERRIDE 0x1dc
#define PMC_PLLM_WB0_OVERRIDE_2 0x2b0

/* Tegra CPU clock and reset control regs */
#define CLK_RST_CONTROLLER_CPU_CMPLX_STATUS	0x470

#ifdef CONFIG_PM_SLEEP
static struct cpu_clk_suspend_context {
	u32 clk_csite_src;
	u32 cclkg_burst;
	u32 cclkg_divider;
} tegra114_cpu_clk_sctx;
#endif

static void __iomem *clk_base;
static void __iomem *pmc_base;

static DEFINE_SPINLOCK(pll_d_lock);
static DEFINE_SPINLOCK(pll_d2_lock);
static DEFINE_SPINLOCK(pll_u_lock);
static DEFINE_SPINLOCK(pll_div_lock);
static DEFINE_SPINLOCK(pll_re_lock);
static DEFINE_SPINLOCK(clk_doubler_lock);
static DEFINE_SPINLOCK(clk_out_lock);
static DEFINE_SPINLOCK(sysrate_lock);

static struct div_nmp pllxc_nmp = {
	.divm_shift = 0,
	.divm_width = 8,
	.divn_shift = 8,
	.divn_width = 8,
	.divp_shift = 20,
	.divp_width = 4,
};

static struct pdiv_map pllxc_p[] = {
	{ .pdiv = 1, .hw_val = 0 },
	{ .pdiv = 2, .hw_val = 1 },
	{ .pdiv = 3, .hw_val = 2 },
	{ .pdiv = 4, .hw_val = 3 },
	{ .pdiv = 5, .hw_val = 4 },
	{ .pdiv = 6, .hw_val = 5 },
	{ .pdiv = 8, .hw_val = 6 },
	{ .pdiv = 10, .hw_val = 7 },
	{ .pdiv = 12, .hw_val = 8 },
	{ .pdiv = 16, .hw_val = 9 },
	{ .pdiv = 12, .hw_val = 10 },
	{ .pdiv = 16, .hw_val = 11 },
	{ .pdiv = 20, .hw_val = 12 },
	{ .pdiv = 24, .hw_val = 13 },
	{ .pdiv = 32, .hw_val = 14 },
	{ .pdiv = 0, .hw_val = 0 },
};

static struct tegra_clk_pll_freq_table pll_c_freq_table[] = {
	{ 12000000, 624000000, 104, 0, 2},
	{ 12000000, 600000000, 100, 0, 2},
	{ 13000000, 600000000,  92, 0, 2},	/* actual: 598.0 MHz */
	{ 16800000, 600000000,  71, 0, 2},	/* actual: 596.4 MHz */
	{ 19200000, 600000000,  62, 0, 2},	/* actual: 595.2 MHz */
	{ 26000000, 600000000,  92, 1, 2},	/* actual: 598.0 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct tegra_clk_pll_params pll_c_params = {
	.input_min = 12000000,
	.input_max = 800000000,
	.cf_min = 12000000,
	.cf_max = 19200000,	/* s/w policy, h/w capability 50 MHz */
	.vco_min = 600000000,
	.vco_max = 1400000000,
	.base_reg = PLLC_BASE,
	.misc_reg = PLLC_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLLC_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.iddq_reg = PLLC_MISC,
	.iddq_bit_idx = PLLC_IDDQ_BIT,
	.max_p = PLLXC_SW_MAX_P,
	.dyn_ramp_reg = PLLC_MISC2,
	.stepa_shift = 17,
	.stepb_shift = 9,
	.pdiv_tohw = pllxc_p,
	.div_nmp = &pllxc_nmp,
	.freq_table = pll_c_freq_table,
	.flags = TEGRA_PLL_USE_LOCK,
};

static struct div_nmp pllcx_nmp = {
	.divm_shift = 0,
	.divm_width = 2,
	.divn_shift = 8,
	.divn_width = 8,
	.divp_shift = 20,
	.divp_width = 3,
};

static struct pdiv_map pllc_p[] = {
	{ .pdiv = 1, .hw_val = 0 },
	{ .pdiv = 2, .hw_val = 1 },
	{ .pdiv = 4, .hw_val = 3 },
	{ .pdiv = 8, .hw_val = 5 },
	{ .pdiv = 16, .hw_val = 7 },
	{ .pdiv = 0, .hw_val = 0 },
};

static struct tegra_clk_pll_freq_table pll_cx_freq_table[] = {
	{12000000, 600000000, 100, 0, 2},
	{13000000, 600000000, 92, 0, 2},	/* actual: 598.0 MHz */
	{16800000, 600000000, 71, 0, 2},	/* actual: 596.4 MHz */
	{19200000, 600000000, 62, 0, 2},	/* actual: 595.2 MHz */
	{26000000, 600000000, 92, 1, 2},	/* actual: 598.0 MHz */
	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_c2_params = {
	.input_min = 12000000,
	.input_max = 48000000,
	.cf_min = 12000000,
	.cf_max = 19200000,
	.vco_min = 600000000,
	.vco_max = 1200000000,
	.base_reg = PLLC2_BASE,
	.misc_reg = PLLC2_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.pdiv_tohw = pllc_p,
	.div_nmp = &pllcx_nmp,
	.max_p = 7,
	.ext_misc_reg[0] = 0x4f0,
	.ext_misc_reg[1] = 0x4f4,
	.ext_misc_reg[2] = 0x4f8,
	.freq_table = pll_cx_freq_table,
	.flags = TEGRA_PLL_USE_LOCK,
};

static struct tegra_clk_pll_params pll_c3_params = {
	.input_min = 12000000,
	.input_max = 48000000,
	.cf_min = 12000000,
	.cf_max = 19200000,
	.vco_min = 600000000,
	.vco_max = 1200000000,
	.base_reg = PLLC3_BASE,
	.misc_reg = PLLC3_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.pdiv_tohw = pllc_p,
	.div_nmp = &pllcx_nmp,
	.max_p = 7,
	.ext_misc_reg[0] = 0x504,
	.ext_misc_reg[1] = 0x508,
	.ext_misc_reg[2] = 0x50c,
	.freq_table = pll_cx_freq_table,
	.flags = TEGRA_PLL_USE_LOCK,
};

static struct div_nmp pllm_nmp = {
	.divm_shift = 0,
	.divm_width = 8,
	.override_divm_shift = 0,
	.divn_shift = 8,
	.divn_width = 8,
	.override_divn_shift = 8,
	.divp_shift = 20,
	.divp_width = 1,
	.override_divp_shift = 27,
};

static struct pdiv_map pllm_p[] = {
	{ .pdiv = 1, .hw_val = 0 },
	{ .pdiv = 2, .hw_val = 1 },
	{ .pdiv = 0, .hw_val = 0 },
};

static struct tegra_clk_pll_freq_table pll_m_freq_table[] = {
	{12000000, 800000000, 66, 0, 1},	/* actual: 792.0 MHz */
	{13000000, 800000000, 61, 0, 1},	/* actual: 793.0 MHz */
	{16800000, 800000000, 47, 0, 1},	/* actual: 789.6 MHz */
	{19200000, 800000000, 41, 0, 1},	/* actual: 787.2 MHz */
	{26000000, 800000000, 61, 1, 1},	/* actual: 793.0 MHz */
	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_m_params = {
	.input_min = 12000000,
	.input_max = 500000000,
	.cf_min = 12000000,
	.cf_max = 19200000,	/* s/w policy, h/w capability 50 MHz */
	.vco_min = 400000000,
	.vco_max = 1066000000,
	.base_reg = PLLM_BASE,
	.misc_reg = PLLM_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.max_p = 2,
	.pdiv_tohw = pllm_p,
	.div_nmp = &pllm_nmp,
	.pmc_divnm_reg = PMC_PLLM_WB0_OVERRIDE,
	.pmc_divp_reg = PMC_PLLM_WB0_OVERRIDE_2,
	.freq_table = pll_m_freq_table,
	.flags = TEGRA_PLL_USE_LOCK,
};

static struct div_nmp pllp_nmp = {
	.divm_shift = 0,
	.divm_width = 5,
	.divn_shift = 8,
	.divn_width = 10,
	.divp_shift = 20,
	.divp_width = 3,
};

static struct tegra_clk_pll_freq_table pll_p_freq_table[] = {
	{12000000, 216000000, 432, 12, 1, 8},
	{13000000, 216000000, 432, 13, 1, 8},
	{16800000, 216000000, 360, 14, 1, 8},
	{19200000, 216000000, 360, 16, 1, 8},
	{26000000, 216000000, 432, 26, 1, 8},
	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_p_params = {
	.input_min = 2000000,
	.input_max = 31000000,
	.cf_min = 1000000,
	.cf_max = 6000000,
	.vco_min = 200000000,
	.vco_max = 700000000,
	.base_reg = PLLP_BASE,
	.misc_reg = PLLP_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.div_nmp = &pllp_nmp,
	.freq_table = pll_p_freq_table,
	.flags = TEGRA_PLL_FIXED | TEGRA_PLL_USE_LOCK,
	.fixed_rate = 408000000,
};

static struct tegra_clk_pll_freq_table pll_a_freq_table[] = {
	{9600000, 282240000, 147, 5, 0, 4},
	{9600000, 368640000, 192, 5, 0, 4},
	{9600000, 240000000, 200, 8, 0, 8},

	{28800000, 282240000, 245, 25, 0, 8},
	{28800000, 368640000, 320, 25, 0, 8},
	{28800000, 240000000, 200, 24, 0, 8},
	{0, 0, 0, 0, 0, 0},
};


static struct tegra_clk_pll_params pll_a_params = {
	.input_min = 2000000,
	.input_max = 31000000,
	.cf_min = 1000000,
	.cf_max = 6000000,
	.vco_min = 200000000,
	.vco_max = 700000000,
	.base_reg = PLLA_BASE,
	.misc_reg = PLLA_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.div_nmp = &pllp_nmp,
	.freq_table = pll_a_freq_table,
	.flags = TEGRA_PLL_HAS_CPCON | TEGRA_PLL_USE_LOCK,
};

static struct tegra_clk_pll_freq_table pll_d_freq_table[] = {
	{12000000, 216000000, 864, 12, 2, 12},
	{13000000, 216000000, 864, 13, 2, 12},
	{16800000, 216000000, 720, 14, 2, 12},
	{19200000, 216000000, 720, 16, 2, 12},
	{26000000, 216000000, 864, 26, 2, 12},

	{12000000, 594000000, 594, 12, 0, 12},
	{13000000, 594000000, 594, 13, 0, 12},
	{16800000, 594000000, 495, 14, 0, 12},
	{19200000, 594000000, 495, 16, 0, 12},
	{26000000, 594000000, 594, 26, 0, 12},

	{12000000, 1000000000, 1000, 12, 0, 12},
	{13000000, 1000000000, 1000, 13, 0, 12},
	{19200000, 1000000000, 625, 12, 0, 12},
	{26000000, 1000000000, 1000, 26, 0, 12},

	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_d_params = {
	.input_min = 2000000,
	.input_max = 40000000,
	.cf_min = 1000000,
	.cf_max = 6000000,
	.vco_min = 500000000,
	.vco_max = 1000000000,
	.base_reg = PLLD_BASE,
	.misc_reg = PLLD_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLLDU_MISC_LOCK_ENABLE,
	.lock_delay = 1000,
	.div_nmp = &pllp_nmp,
	.freq_table = pll_d_freq_table,
	.flags = TEGRA_PLL_HAS_CPCON | TEGRA_PLL_SET_LFCON |
		 TEGRA_PLL_USE_LOCK,
};

static struct tegra_clk_pll_params pll_d2_params = {
	.input_min = 2000000,
	.input_max = 40000000,
	.cf_min = 1000000,
	.cf_max = 6000000,
	.vco_min = 500000000,
	.vco_max = 1000000000,
	.base_reg = PLLD2_BASE,
	.misc_reg = PLLD2_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLLDU_MISC_LOCK_ENABLE,
	.lock_delay = 1000,
	.div_nmp = &pllp_nmp,
	.freq_table = pll_d_freq_table,
	.flags = TEGRA_PLL_HAS_CPCON | TEGRA_PLL_SET_LFCON |
		 TEGRA_PLL_USE_LOCK,
};

static struct pdiv_map pllu_p[] = {
	{ .pdiv = 1, .hw_val = 1 },
	{ .pdiv = 2, .hw_val = 0 },
	{ .pdiv = 0, .hw_val = 0 },
};

static struct div_nmp pllu_nmp = {
	.divm_shift = 0,
	.divm_width = 5,
	.divn_shift = 8,
	.divn_width = 10,
	.divp_shift = 20,
	.divp_width = 1,
};

static struct tegra_clk_pll_freq_table pll_u_freq_table[] = {
	{12000000, 480000000, 960, 12, 0, 12},
	{13000000, 480000000, 960, 13, 0, 12},
	{16800000, 480000000, 400, 7, 0, 5},
	{19200000, 480000000, 200, 4, 0, 3},
	{26000000, 480000000, 960, 26, 0, 12},
	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_u_params = {
	.input_min = 2000000,
	.input_max = 40000000,
	.cf_min = 1000000,
	.cf_max = 6000000,
	.vco_min = 480000000,
	.vco_max = 960000000,
	.base_reg = PLLU_BASE,
	.misc_reg = PLLU_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLLDU_MISC_LOCK_ENABLE,
	.lock_delay = 1000,
	.pdiv_tohw = pllu_p,
	.div_nmp = &pllu_nmp,
	.freq_table = pll_u_freq_table,
	.flags = TEGRA_PLLU | TEGRA_PLL_HAS_CPCON | TEGRA_PLL_SET_LFCON |
		 TEGRA_PLL_USE_LOCK,
};

static struct tegra_clk_pll_freq_table pll_x_freq_table[] = {
	/* 1 GHz */
	{12000000, 1000000000, 83, 0, 1},	/* actual: 996.0 MHz */
	{13000000, 1000000000, 76, 0, 1},	/* actual: 988.0 MHz */
	{16800000, 1000000000, 59, 0, 1},	/* actual: 991.2 MHz */
	{19200000, 1000000000, 52, 0, 1},	/* actual: 998.4 MHz */
	{26000000, 1000000000, 76, 1, 1},	/* actual: 988.0 MHz */

	{0, 0, 0, 0, 0, 0},
};

static struct tegra_clk_pll_params pll_x_params = {
	.input_min = 12000000,
	.input_max = 800000000,
	.cf_min = 12000000,
	.cf_max = 19200000,	/* s/w policy, h/w capability 50 MHz */
	.vco_min = 700000000,
	.vco_max = 2400000000U,
	.base_reg = PLLX_BASE,
	.misc_reg = PLLX_MISC,
	.lock_mask = PLL_BASE_LOCK,
	.lock_enable_bit_idx = PLL_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.iddq_reg = PLLX_MISC3,
	.iddq_bit_idx = PLLX_IDDQ_BIT,
	.max_p = PLLXC_SW_MAX_P,
	.dyn_ramp_reg = PLLX_MISC2,
	.stepa_shift = 16,
	.stepb_shift = 24,
	.pdiv_tohw = pllxc_p,
	.div_nmp = &pllxc_nmp,
	.freq_table = pll_x_freq_table,
	.flags = TEGRA_PLL_USE_LOCK,
};

static struct tegra_clk_pll_freq_table pll_e_freq_table[] = {
	/* PLLE special case: use cpcon field to store cml divider value */
	{336000000, 100000000, 100, 21, 16, 11},
	{312000000, 100000000, 200, 26, 24, 13},
	{12000000, 100000000, 200,  1,  24, 13},
	{0, 0, 0, 0, 0, 0},
};

static struct div_nmp plle_nmp = {
	.divm_shift = 0,
	.divm_width = 8,
	.divn_shift = 8,
	.divn_width = 8,
	.divp_shift = 24,
	.divp_width = 4,
};

static struct tegra_clk_pll_params pll_e_params = {
	.input_min = 12000000,
	.input_max = 1000000000,
	.cf_min = 12000000,
	.cf_max = 75000000,
	.vco_min = 1600000000,
	.vco_max = 2400000000U,
	.base_reg = PLLE_BASE,
	.misc_reg = PLLE_MISC,
	.aux_reg = PLLE_AUX,
	.lock_mask = PLLE_MISC_LOCK,
	.lock_enable_bit_idx = PLLE_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.div_nmp = &plle_nmp,
	.freq_table = pll_e_freq_table,
	.flags = TEGRA_PLL_FIXED,
	.fixed_rate = 100000000,
};

static struct div_nmp pllre_nmp = {
	.divm_shift = 0,
	.divm_width = 8,
	.divn_shift = 8,
	.divn_width = 8,
	.divp_shift = 16,
	.divp_width = 4,
};

static struct tegra_clk_pll_params pll_re_vco_params = {
	.input_min = 12000000,
	.input_max = 1000000000,
	.cf_min = 12000000,
	.cf_max = 19200000, /* s/w policy, h/w capability 38 MHz */
	.vco_min = 300000000,
	.vco_max = 600000000,
	.base_reg = PLLRE_BASE,
	.misc_reg = PLLRE_MISC,
	.lock_mask = PLLRE_MISC_LOCK,
	.lock_enable_bit_idx = PLLRE_MISC_LOCK_ENABLE,
	.lock_delay = 300,
	.iddq_reg = PLLRE_MISC,
	.iddq_bit_idx = PLLRE_IDDQ_BIT,
	.div_nmp = &pllre_nmp,
	.flags = TEGRA_PLL_USE_LOCK,
};

/* possible OSC frequencies in Hz */
static unsigned long tegra114_input_freq[] = {
	[0] = 13000000,
	[1] = 16800000,
	[4] = 19200000,
	[5] = 38400000,
	[8] = 12000000,
	[9] = 48000000,
	[12] = 260000000,
};

#define MASK(x) (BIT(x) - 1)

#define TEGRA_INIT_DATA_MUX(_name, _parents, _offset,	\
			    _clk_num, _gate_flags, _clk_id)	\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			30, MASK(2), 0, 0, 8, 1, TEGRA_DIVIDER_ROUND_UP, \
			_clk_num, _gate_flags, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_MUX_FLAGS(_name, _parents, _offset,\
			    _clk_num, _gate_flags, _clk_id, flags)\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			30, MASK(2), 0, 0, 8, 1, TEGRA_DIVIDER_ROUND_UP,\
			_clk_num, _gate_flags, _clk_id, _parents##_idx, flags)

#define TEGRA_INIT_DATA_MUX8(_name, _parents, _offset, \
			     _clk_num, _gate_flags, _clk_id)	\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			29, MASK(3), 0, 0, 8, 1, TEGRA_DIVIDER_ROUND_UP,\
			_clk_num, _gate_flags, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_INT_FLAGS(_name, _parents, _offset,\
			    _clk_num, _gate_flags, _clk_id, flags)\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			30, MASK(2), 0, 0, 8, 1, TEGRA_DIVIDER_INT |	\
			TEGRA_DIVIDER_ROUND_UP, _clk_num,		\
			_gate_flags, _clk_id, _parents##_idx, flags)

#define TEGRA_INIT_DATA_INT8(_name, _parents, _offset,\
			    _clk_num, _gate_flags, _clk_id)	\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			29, MASK(3), 0, 0, 8, 1, TEGRA_DIVIDER_INT |	\
			TEGRA_DIVIDER_ROUND_UP, _clk_num,		\
			_gate_flags, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_UART(_name, _parents, _offset,\
			     _clk_num, _clk_id)			\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			30, MASK(2), 0, 0, 16, 1, TEGRA_DIVIDER_UART |	\
			TEGRA_DIVIDER_ROUND_UP, _clk_num,		\
			0, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_I2C(_name, _parents, _offset,\
			     _clk_num, _clk_id)			\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			30, MASK(2), 0, 0, 16, 0, TEGRA_DIVIDER_ROUND_UP,\
			_clk_num,  0, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_NODIV(_name, _parents, _offset, \
			      _mux_shift, _mux_mask, _clk_num, \
			      _gate_flags, _clk_id)			\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset,\
			_mux_shift, _mux_mask, 0, 0, 0, 0, 0,\
			_clk_num, _gate_flags,	\
			_clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_XUSB(_name, _parents, _offset, \
			     _clk_num, _gate_flags, _clk_id)	 \
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, _parents, _offset, \
			29, MASK(3), 0, 0, 8, 1, TEGRA_DIVIDER_INT |	\
			TEGRA_DIVIDER_ROUND_UP, _clk_num,		\
			_gate_flags, _clk_id, _parents##_idx, 0)

#define TEGRA_INIT_DATA_AUDIO(_name, _offset,  _clk_num,\
				 _gate_flags, _clk_id)		\
	TEGRA_INIT_DATA_TABLE(_name, NULL, NULL, mux_d_audio_clk,	\
			_offset, 16, 0xE01F, 0, 0, 8, 1,		\
			TEGRA_DIVIDER_ROUND_UP, _clk_num, 		\
			_gate_flags , _clk_id,	mux_d_audio_clk_idx, 0)

struct utmi_clk_param {
	/* Oscillator Frequency in KHz */
	u32 osc_frequency;
	/* UTMIP PLL Enable Delay Count  */
	u8 enable_delay_count;
	/* UTMIP PLL Stable count */
	u8 stable_count;
	/*  UTMIP PLL Active delay count */
	u8 active_delay_count;
	/* UTMIP PLL Xtal frequency count */
	u8 xtal_freq_count;
};

static const struct utmi_clk_param utmi_parameters[] = {
	{.osc_frequency = 13000000, .enable_delay_count = 0x02,
	 .stable_count = 0x33, .active_delay_count = 0x05,
	 .xtal_freq_count = 0x7F},
	{.osc_frequency = 19200000, .enable_delay_count = 0x03,
	 .stable_count = 0x4B, .active_delay_count = 0x06,
	 .xtal_freq_count = 0xBB},
	{.osc_frequency = 12000000, .enable_delay_count = 0x02,
	 .stable_count = 0x2F, .active_delay_count = 0x04,
	 .xtal_freq_count = 0x76},
	{.osc_frequency = 26000000, .enable_delay_count = 0x04,
	 .stable_count = 0x66, .active_delay_count = 0x09,
	 .xtal_freq_count = 0xFE},
	{.osc_frequency = 16800000, .enable_delay_count = 0x03,
	 .stable_count = 0x41, .active_delay_count = 0x0A,
	 .xtal_freq_count = 0xA4},
};

/* peripheral mux definitions */

#define MUX_I2S_SPDIF(_id)						\
static const char *mux_pllaout0_##_id##_2x_pllp_clkm[] = { "pll_a_out0", \
							   #_id, "pll_p",\
							   "clk_m"};
MUX_I2S_SPDIF(audio0)
MUX_I2S_SPDIF(audio1)
MUX_I2S_SPDIF(audio2)
MUX_I2S_SPDIF(audio3)
MUX_I2S_SPDIF(audio4)
MUX_I2S_SPDIF(audio)

#define mux_pllaout0_audio0_2x_pllp_clkm_idx NULL
#define mux_pllaout0_audio1_2x_pllp_clkm_idx NULL
#define mux_pllaout0_audio2_2x_pllp_clkm_idx NULL
#define mux_pllaout0_audio3_2x_pllp_clkm_idx NULL
#define mux_pllaout0_audio4_2x_pllp_clkm_idx NULL
#define mux_pllaout0_audio_2x_pllp_clkm_idx NULL

static const char *mux_pllp_pllc_pllm_clkm[] = {
	"pll_p", "pll_c", "pll_m", "clk_m"
};
#define mux_pllp_pllc_pllm_clkm_idx NULL

static const char *mux_pllp_pllc_pllm[] = { "pll_p", "pll_c", "pll_m" };
#define mux_pllp_pllc_pllm_idx NULL

static const char *mux_pllp_pllc_clk32_clkm[] = {
	"pll_p", "pll_c", "clk_32k", "clk_m"
};
#define mux_pllp_pllc_clk32_clkm_idx NULL

static const char *mux_plla_pllc_pllp_clkm[] = {
	"pll_a_out0", "pll_c", "pll_p", "clk_m"
};
#define mux_plla_pllc_pllp_clkm_idx mux_pllp_pllc_pllm_clkm_idx

static const char *mux_pllp_pllc2_c_c3_pllm_clkm[] = {
	"pll_p", "pll_c2", "pll_c", "pll_c3", "pll_m", "clk_m"
};
static u32 mux_pllp_pllc2_c_c3_pllm_clkm_idx[] = {
	[0] = 0, [1] = 1, [2] = 2, [3] = 3, [4] = 4, [5] = 6,
};

static const char *mux_pllp_clkm[] = {
	"pll_p", "clk_m"
};
static u32 mux_pllp_clkm_idx[] = {
	[0] = 0, [1] = 3,
};

static const char *mux_pllm_pllc2_c_c3_pllp_plla[] = {
	"pll_m", "pll_c2", "pll_c", "pll_c3", "pll_p", "pll_a_out0"
};
#define mux_pllm_pllc2_c_c3_pllp_plla_idx mux_pllp_pllc2_c_c3_pllm_clkm_idx

static const char *mux_pllp_pllm_plld_plla_pllc_plld2_clkm[] = {
	"pll_p", "pll_m", "pll_d_out0", "pll_a_out0", "pll_c",
	"pll_d2_out0", "clk_m"
};
#define mux_pllp_pllm_plld_plla_pllc_plld2_clkm_idx NULL

static const char *mux_pllm_pllc_pllp_plla[] = {
	"pll_m", "pll_c", "pll_p", "pll_a_out0"
};
#define mux_pllm_pllc_pllp_plla_idx mux_pllp_pllc_pllm_clkm_idx

static const char *mux_pllp_pllc_clkm[] = {
	"pll_p", "pll_c", "pll_m"
};
static u32 mux_pllp_pllc_clkm_idx[] = {
	[0] = 0, [1] = 1, [2] = 3,
};

static const char *mux_pllp_pllc_clkm_clk32[] = {
	"pll_p", "pll_c", "clk_m", "clk_32k"
};
#define mux_pllp_pllc_clkm_clk32_idx NULL

static const char *mux_plla_clk32_pllp_clkm_plle[] = {
	"pll_a_out0", "clk_32k", "pll_p", "clk_m", "pll_e_out0"
};
#define mux_plla_clk32_pllp_clkm_plle_idx NULL

static const char *mux_clkm_pllp_pllc_pllre[] = {
	"clk_m", "pll_p", "pll_c", "pll_re_out"
};
static u32 mux_clkm_pllp_pllc_pllre_idx[] = {
	[0] = 0, [1] = 1, [2] = 3, [3] = 5,
};

static const char *mux_clkm_48M_pllp_480M[] = {
	"clk_m", "pll_u_48M", "pll_p", "pll_u_480M"
};
#define mux_clkm_48M_pllp_480M_idx NULL

static const char *mux_clkm_pllre_clk32_480M_pllc_ref[] = {
	"clk_m", "pll_re_out", "clk_32k", "pll_u_480M", "pll_c", "pll_ref"
};
static u32 mux_clkm_pllre_clk32_480M_pllc_ref_idx[] = {
	[0] = 0, [1] = 1, [2] = 3, [3] = 3, [4] = 4, [5] = 7,
};

static const char *mux_plld_out0_plld2_out0[] = {
	"pll_d_out0", "pll_d2_out0",
};
#define mux_plld_out0_plld2_out0_idx NULL

static const char *mux_d_audio_clk[] = {
	"pll_a_out0", "pll_p", "clk_m", "spdif_in_sync", "i2s0_sync",
	"i2s1_sync", "i2s2_sync", "i2s3_sync", "i2s4_sync", "vimclk_sync",
};
static u32 mux_d_audio_clk_idx[] = {
	[0] = 0, [1] = 0x8000, [2] = 0xc000, [3] = 0xE000, [4] = 0xE001,
	[5] = 0xE002, [6] = 0xE003, [7] = 0xE004, [8] = 0xE005, [9] = 0xE007,
};

static const char *mux_pllmcp_clkm[] = {
	"pll_m_out0", "pll_c_out0", "pll_p_out0", "clk_m", "pll_m_ud",
};

static const struct clk_div_table pll_re_div_table[] = {
	{ .val = 0, .div = 1 },
	{ .val = 1, .div = 2 },
	{ .val = 2, .div = 3 },
	{ .val = 3, .div = 4 },
	{ .val = 4, .div = 5 },
	{ .val = 5, .div = 6 },
	{ .val = 0, .div = 0 },
};

static struct tegra_devclk devclks[] __initdata = {
	{ .con_id = "clk_m", .dt_id = TEGRA114_CLK_CLK_M },
	{ .con_id = "pll_ref", .dt_id = TEGRA114_CLK_PLL_REF },
	{ .con_id = "clk_32k", .dt_id = TEGRA114_CLK_CLK_32K },
	{ .con_id = "clk_m_div2", .dt_id = TEGRA114_CLK_CLK_M_DIV2 },
	{ .con_id = "clk_m_div4", .dt_id = TEGRA114_CLK_CLK_M_DIV4 },
	{ .con_id = "pll_c", .dt_id = TEGRA114_CLK_PLL_C },
	{ .con_id = "pll_c_out1", .dt_id = TEGRA114_CLK_PLL_C_OUT1 },
	{ .con_id = "pll_c2", .dt_id = TEGRA114_CLK_PLL_C2 },
	{ .con_id = "pll_c3", .dt_id = TEGRA114_CLK_PLL_C3 },
	{ .con_id = "pll_p", .dt_id = TEGRA114_CLK_PLL_P },
	{ .con_id = "pll_p_out1", .dt_id = TEGRA114_CLK_PLL_P_OUT1 },
	{ .con_id = "pll_p_out2", .dt_id = TEGRA114_CLK_PLL_P_OUT2 },
	{ .con_id = "pll_p_out3", .dt_id = TEGRA114_CLK_PLL_P_OUT3 },
	{ .con_id = "pll_p_out4", .dt_id = TEGRA114_CLK_PLL_P_OUT4 },
	{ .con_id = "pll_m", .dt_id = TEGRA114_CLK_PLL_M },
	{ .con_id = "pll_m_out1", .dt_id = TEGRA114_CLK_PLL_M_OUT1 },
	{ .con_id = "pll_x", .dt_id = TEGRA114_CLK_PLL_X },
	{ .con_id = "pll_x_out0", .dt_id = TEGRA114_CLK_PLL_X_OUT0 },
	{ .con_id = "pll_u", .dt_id = TEGRA114_CLK_PLL_U },
	{ .con_id = "pll_u_480M", .dt_id = TEGRA114_CLK_PLL_U_480M },
	{ .con_id = "pll_u_60M", .dt_id = TEGRA114_CLK_PLL_U_60M },
	{ .con_id = "pll_u_48M", .dt_id = TEGRA114_CLK_PLL_U_48M },
	{ .con_id = "pll_u_12M", .dt_id = TEGRA114_CLK_PLL_U_12M },
	{ .con_id = "pll_d", .dt_id = TEGRA114_CLK_PLL_D },
	{ .con_id = "pll_d_out0", .dt_id = TEGRA114_CLK_PLL_D_OUT0 },
	{ .con_id = "pll_d2", .dt_id = TEGRA114_CLK_PLL_D2 },
	{ .con_id = "pll_d2_out0", .dt_id = TEGRA114_CLK_PLL_D2_OUT0 },
	{ .con_id = "pll_a", .dt_id = TEGRA114_CLK_PLL_A },
	{ .con_id = "pll_a_out0", .dt_id = TEGRA114_CLK_PLL_A_OUT0 },
	{ .con_id = "pll_re_vco", .dt_id = TEGRA114_CLK_PLL_RE_VCO },
	{ .con_id = "pll_re_out", .dt_id = TEGRA114_CLK_PLL_RE_OUT },
	{ .con_id = "pll_e_out0", .dt_id = TEGRA114_CLK_PLL_E_OUT0 },
	{ .con_id = "spdif_in_sync", .dt_id = TEGRA114_CLK_SPDIF_IN_SYNC },
	{ .con_id = "i2s0_sync", .dt_id = TEGRA114_CLK_I2S0_SYNC },
	{ .con_id = "i2s1_sync", .dt_id = TEGRA114_CLK_I2S1_SYNC },
	{ .con_id = "i2s2_sync", .dt_id = TEGRA114_CLK_I2S2_SYNC },
	{ .con_id = "i2s3_sync", .dt_id = TEGRA114_CLK_I2S3_SYNC },
	{ .con_id = "i2s4_sync", .dt_id = TEGRA114_CLK_I2S4_SYNC },
	{ .con_id = "vimclk_sync", .dt_id = TEGRA114_CLK_VIMCLK_SYNC },
	{ .con_id = "audio0", .dt_id = TEGRA114_CLK_AUDIO0 },
	{ .con_id = "audio1", .dt_id = TEGRA114_CLK_AUDIO1 },
	{ .con_id = "audio2", .dt_id = TEGRA114_CLK_AUDIO2 },
	{ .con_id = "audio3", .dt_id = TEGRA114_CLK_AUDIO3 },
	{ .con_id = "audio4", .dt_id = TEGRA114_CLK_AUDIO4 },
	{ .con_id = "spdif", .dt_id = TEGRA114_CLK_SPDIF },
	{ .con_id = "audio0_2x", .dt_id = TEGRA114_CLK_AUDIO0_2X },
	{ .con_id = "audio1_2x", .dt_id = TEGRA114_CLK_AUDIO1_2X },
	{ .con_id = "audio2_2x", .dt_id = TEGRA114_CLK_AUDIO2_2X },
	{ .con_id = "audio3_2x", .dt_id = TEGRA114_CLK_AUDIO3_2X },
	{ .con_id = "audio4_2x", .dt_id = TEGRA114_CLK_AUDIO4_2X },
	{ .con_id = "spdif_2x", .dt_id = TEGRA114_CLK_SPDIF_2X },
	{ .con_id = "extern1", .dev_id = "clk_out_1", .dt_id = TEGRA114_CLK_EXTERN1 },
	{ .con_id = "extern2", .dev_id = "clk_out_2", .dt_id = TEGRA114_CLK_EXTERN2 },
	{ .con_id = "extern3", .dev_id = "clk_out_3", .dt_id = TEGRA114_CLK_EXTERN3 },
	{ .con_id = "blink", .dt_id = TEGRA114_CLK_BLINK },
	{ .con_id = "cclk_g", .dt_id = TEGRA114_CLK_CCLK_G },
	{ .con_id = "cclk_lp", .dt_id = TEGRA114_CLK_CCLK_LP },
	{ .con_id = "sclk", .dt_id = TEGRA114_CLK_SCLK },
	{ .con_id = "hclk", .dt_id = TEGRA114_CLK_HCLK },
	{ .con_id = "pclk", .dt_id = TEGRA114_CLK_PCLK },
	{ .dev_id = "rtc-tegra", .dt_id = TEGRA114_CLK_RTC },
	{ .dev_id = "timer", .dt_id = TEGRA114_CLK_TIMER },
};

static struct clk **clks;

static unsigned long osc_freq;
static unsigned long pll_ref_freq;

static int __init tegra114_osc_clk_init(void __iomem *clk_base)
{
	struct clk *clk;
	u32 val, pll_ref_div;

	val = readl_relaxed(clk_base + OSC_CTRL);

	osc_freq = tegra114_input_freq[val >> OSC_CTRL_OSC_FREQ_SHIFT];
	if (!osc_freq) {
		WARN_ON(1);
		return -EINVAL;
	}

	/* clk_m */
	clk = clk_register_fixed_rate(NULL, "clk_m", NULL, CLK_IS_ROOT,
				      osc_freq);
	clks[TEGRA114_CLK_CLK_M] = clk;

	/* pll_ref */
	val = (val >> OSC_CTRL_PLL_REF_DIV_SHIFT) & 3;
	pll_ref_div = 1 << val;
	clk = clk_register_fixed_factor(NULL, "pll_ref", "clk_m",
					CLK_SET_RATE_PARENT, 1, pll_ref_div);
	clks[TEGRA114_CLK_PLL_REF] = clk;

	pll_ref_freq = osc_freq / pll_ref_div;

	return 0;
}

static void __init tegra114_fixed_clk_init(void __iomem *clk_base)
{
	struct clk *clk;

	/* clk_32k */
	clk = clk_register_fixed_rate(NULL, "clk_32k", NULL, CLK_IS_ROOT,
				      32768);
	clks[TEGRA114_CLK_CLK_32K] = clk;

	/* clk_m_div2 */
	clk = clk_register_fixed_factor(NULL, "clk_m_div2", "clk_m",
					CLK_SET_RATE_PARENT, 1, 2);
	clks[TEGRA114_CLK_CLK_M_DIV2] = clk;

	/* clk_m_div4 */
	clk = clk_register_fixed_factor(NULL, "clk_m_div4", "clk_m",
					CLK_SET_RATE_PARENT, 1, 4);
	clks[TEGRA114_CLK_CLK_M_DIV4] = clk;

}

static __init void tegra114_utmi_param_configure(void __iomem *clk_base)
{
	u32 reg;
	int i;

	for (i = 0; i < ARRAY_SIZE(utmi_parameters); i++) {
		if (osc_freq == utmi_parameters[i].osc_frequency)
			break;
	}

	if (i >= ARRAY_SIZE(utmi_parameters)) {
		pr_err("%s: Unexpected oscillator freq %lu\n", __func__,
		       osc_freq);
		return;
	}

	reg = readl_relaxed(clk_base + UTMIP_PLL_CFG2);

	/* Program UTMIP PLL stable and active counts */
	/* [FIXME] arclk_rst.h says WRONG! This should be 1ms -> 0x50 Check! */
	reg &= ~UTMIP_PLL_CFG2_STABLE_COUNT(~0);
	reg |= UTMIP_PLL_CFG2_STABLE_COUNT(utmi_parameters[i].stable_count);

	reg &= ~UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(utmi_parameters[i].
					    active_delay_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN;

	writel_relaxed(reg, clk_base + UTMIP_PLL_CFG2);

	/* Program UTMIP PLL delay and oscillator frequency counts */
	reg = readl_relaxed(clk_base + UTMIP_PLL_CFG1);
	reg &= ~UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(utmi_parameters[i].
					    enable_delay_count);

	reg &= ~UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(~0);
	reg |= UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(utmi_parameters[i].
					   xtal_freq_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLLU_POWERUP;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN;
	writel_relaxed(reg, clk_base + UTMIP_PLL_CFG1);

	/* Setup HW control of UTMIPLL */
	reg = readl_relaxed(clk_base + UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_USE_LOCKDET;
	reg &= ~UTMIPLL_HW_PWRDN_CFG0_CLK_ENABLE_SWCTL;
	reg |= UTMIPLL_HW_PWRDN_CFG0_SEQ_START_STATE;
	writel_relaxed(reg, clk_base + UTMIPLL_HW_PWRDN_CFG0);

	reg = readl_relaxed(clk_base + UTMIP_PLL_CFG1);
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERUP;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN;
	writel_relaxed(reg, clk_base + UTMIP_PLL_CFG1);

	udelay(1);

	/* Setup SW override of UTMIPLL assuming USB2.0
	   ports are assigned to USB2 */
	reg = readl_relaxed(clk_base + UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_IDDQ_SWCTL;
	reg &= ~UTMIPLL_HW_PWRDN_CFG0_IDDQ_OVERRIDE;
	writel_relaxed(reg, clk_base + UTMIPLL_HW_PWRDN_CFG0);

	udelay(1);

	/* Enable HW control UTMIPLL */
	reg = readl_relaxed(clk_base + UTMIPLL_HW_PWRDN_CFG0);
	reg |= UTMIPLL_HW_PWRDN_CFG0_SEQ_ENABLE;
	writel_relaxed(reg, clk_base + UTMIPLL_HW_PWRDN_CFG0);
}

static void __init tegra114_pll_init(void __iomem *clk_base,
				     void __iomem *pmc)
{
	u32 val;
	struct clk *clk;

	/* PLLC */
	clk = tegra_clk_register_pllxc("pll_c", "pll_ref", clk_base,
			pmc, 0, &pll_c_params, NULL);
	clks[TEGRA114_CLK_PLL_C] = clk;

	/* PLLC_OUT1 */
	clk = tegra_clk_register_divider("pll_c_out1_div", "pll_c",
			clk_base + PLLC_OUT, 0, TEGRA_DIVIDER_ROUND_UP,
			8, 8, 1, NULL);
	clk = tegra_clk_register_pll_out("pll_c_out1", "pll_c_out1_div",
				clk_base + PLLC_OUT, 1, 0,
				CLK_SET_RATE_PARENT, 0, NULL);
	clks[TEGRA114_CLK_PLL_C_OUT1] = clk;

	/* PLLC2 */
	clk = tegra_clk_register_pllc("pll_c2", "pll_ref", clk_base, pmc, 0,
			     &pll_c2_params, NULL);
	clks[TEGRA114_CLK_PLL_C2] = clk;

	/* PLLC3 */
	clk = tegra_clk_register_pllc("pll_c3", "pll_ref", clk_base, pmc, 0,
			     &pll_c3_params, NULL);
	clks[TEGRA114_CLK_PLL_C3] = clk;

	/* PLLP */
	clk = tegra_clk_register_pll("pll_p", "pll_ref", clk_base, pmc, 0,
			    &pll_p_params, NULL);
	clks[TEGRA114_CLK_PLL_P] = clk;

	/* PLLP_OUT1 */
	clk = tegra_clk_register_divider("pll_p_out1_div", "pll_p",
				clk_base + PLLP_OUTA, 0, TEGRA_DIVIDER_FIXED |
				TEGRA_DIVIDER_ROUND_UP, 8, 8, 1, &pll_div_lock);
	clk = tegra_clk_register_pll_out("pll_p_out1", "pll_p_out1_div",
				clk_base + PLLP_OUTA, 1, 0,
				CLK_IGNORE_UNUSED | CLK_SET_RATE_PARENT, 0,
				&pll_div_lock);
	clks[TEGRA114_CLK_PLL_P_OUT1] = clk;

	/* PLLP_OUT2 */
	clk = tegra_clk_register_divider("pll_p_out2_div", "pll_p",
				clk_base + PLLP_OUTA, 0, TEGRA_DIVIDER_FIXED |
				TEGRA_DIVIDER_ROUND_UP | TEGRA_DIVIDER_INT, 24,
				8, 1, &pll_div_lock);
	clk = tegra_clk_register_pll_out("pll_p_out2", "pll_p_out2_div",
				clk_base + PLLP_OUTA, 17, 16,
				CLK_IGNORE_UNUSED | CLK_SET_RATE_PARENT, 0,
				&pll_div_lock);
	clks[TEGRA114_CLK_PLL_P_OUT2] = clk;

	/* PLLP_OUT3 */
	clk = tegra_clk_register_divider("pll_p_out3_div", "pll_p",
				clk_base + PLLP_OUTB, 0, TEGRA_DIVIDER_FIXED |
				TEGRA_DIVIDER_ROUND_UP, 8, 8, 1, &pll_div_lock);
	clk = tegra_clk_register_pll_out("pll_p_out3", "pll_p_out3_div",
				clk_base + PLLP_OUTB, 1, 0,
				CLK_IGNORE_UNUSED | CLK_SET_RATE_PARENT, 0,
				&pll_div_lock);
	clks[TEGRA114_CLK_PLL_P_OUT3] = clk;

	/* PLLP_OUT4 */
	clk = tegra_clk_register_divider("pll_p_out4_div", "pll_p",
				clk_base + PLLP_OUTB, 0, TEGRA_DIVIDER_FIXED |
				TEGRA_DIVIDER_ROUND_UP, 24, 8, 1,
				&pll_div_lock);
	clk = tegra_clk_register_pll_out("pll_p_out4", "pll_p_out4_div",
				clk_base + PLLP_OUTB, 17, 16,
				CLK_IGNORE_UNUSED | CLK_SET_RATE_PARENT, 0,
				&pll_div_lock);
	clks[TEGRA114_CLK_PLL_P_OUT4] = clk;

	/* PLLM */
	clk = tegra_clk_register_pllm("pll_m", "pll_ref", clk_base, pmc,
			     CLK_IGNORE_UNUSED | CLK_SET_RATE_GATE,
			     &pll_m_params, NULL);
	clks[TEGRA114_CLK_PLL_M] = clk;

	/* PLLM_OUT1 */
	clk = tegra_clk_register_divider("pll_m_out1_div", "pll_m",
				clk_base + PLLM_OUT, 0, TEGRA_DIVIDER_ROUND_UP,
				8, 8, 1, NULL);
	clk = tegra_clk_register_pll_out("pll_m_out1", "pll_m_out1_div",
				clk_base + PLLM_OUT, 1, 0, CLK_IGNORE_UNUSED |
				CLK_SET_RATE_PARENT, 0, NULL);
	clks[TEGRA114_CLK_PLL_M_OUT1] = clk;

	/* PLLM_UD */
	clk = clk_register_fixed_factor(NULL, "pll_m_ud", "pll_m",
					CLK_SET_RATE_PARENT, 1, 1);

	/* PLLX */
	clk = tegra_clk_register_pllxc("pll_x", "pll_ref", clk_base,
			pmc, CLK_IGNORE_UNUSED, &pll_x_params, NULL);
	clks[TEGRA114_CLK_PLL_X] = clk;

	/* PLLX_OUT0 */
	clk = clk_register_fixed_factor(NULL, "pll_x_out0", "pll_x",
					CLK_SET_RATE_PARENT, 1, 2);
	clks[TEGRA114_CLK_PLL_X_OUT0] = clk;

	/* PLLU */
	val = readl(clk_base + pll_u_params.base_reg);
	val &= ~BIT(24); /* disable PLLU_OVERRIDE */
	writel(val, clk_base + pll_u_params.base_reg);

	clk = tegra_clk_register_pll("pll_u", "pll_ref", clk_base, pmc, 0,
			    &pll_u_params, &pll_u_lock);
	clks[TEGRA114_CLK_PLL_U] = clk;

	tegra114_utmi_param_configure(clk_base);

	/* PLLU_480M */
	clk = clk_register_gate(NULL, "pll_u_480M", "pll_u",
				CLK_SET_RATE_PARENT, clk_base + PLLU_BASE,
				22, 0, &pll_u_lock);
	clks[TEGRA114_CLK_PLL_U_480M] = clk;

	/* PLLU_60M */
	clk = clk_register_fixed_factor(NULL, "pll_u_60M", "pll_u",
					CLK_SET_RATE_PARENT, 1, 8);
	clks[TEGRA114_CLK_PLL_U_60M] = clk;

	/* PLLU_48M */
	clk = clk_register_fixed_factor(NULL, "pll_u_48M", "pll_u",
					CLK_SET_RATE_PARENT, 1, 10);
	clks[TEGRA114_CLK_PLL_U_48M] = clk;

	/* PLLU_12M */
	clk = clk_register_fixed_factor(NULL, "pll_u_12M", "pll_u",
					CLK_SET_RATE_PARENT, 1, 40);
	clks[TEGRA114_CLK_PLL_U_12M] = clk;

	/* PLLD */
	clk = tegra_clk_register_pll("pll_d", "pll_ref", clk_base, pmc, 0,
			    &pll_d_params, &pll_d_lock);
	clks[TEGRA114_CLK_PLL_D] = clk;

	/* PLLD_OUT0 */
	clk = clk_register_fixed_factor(NULL, "pll_d_out0", "pll_d",
					CLK_SET_RATE_PARENT, 1, 2);
	clks[TEGRA114_CLK_PLL_D_OUT0] = clk;

	/* PLLD2 */
	clk = tegra_clk_register_pll("pll_d2", "pll_ref", clk_base, pmc, 0,
			    &pll_d2_params, &pll_d2_lock);
	clks[TEGRA114_CLK_PLL_D2] = clk;

	/* PLLD2_OUT0 */
	clk = clk_register_fixed_factor(NULL, "pll_d2_out0", "pll_d2",
					CLK_SET_RATE_PARENT, 1, 2);
	clks[TEGRA114_CLK_PLL_D2_OUT0] = clk;

	/* PLLA */
	clk = tegra_clk_register_pll("pll_a", "pll_p_out1", clk_base, pmc, 0,
			    &pll_a_params, NULL);
	clks[TEGRA114_CLK_PLL_A] = clk;

	/* PLLA_OUT0 */
	clk = tegra_clk_register_divider("pll_a_out0_div", "pll_a",
				clk_base + PLLA_OUT, 0, TEGRA_DIVIDER_ROUND_UP,
				8, 8, 1, NULL);
	clk = tegra_clk_register_pll_out("pll_a_out0", "pll_a_out0_div",
				clk_base + PLLA_OUT, 1, 0, CLK_IGNORE_UNUSED |
				CLK_SET_RATE_PARENT, 0, NULL);
	clks[TEGRA114_CLK_PLL_A_OUT0] = clk;

	/* PLLRE */
	clk = tegra_clk_register_pllre("pll_re_vco", "pll_ref", clk_base, pmc,
			     0, &pll_re_vco_params, &pll_re_lock, pll_ref_freq);
	clks[TEGRA114_CLK_PLL_RE_VCO] = clk;

	clk = clk_register_divider_table(NULL, "pll_re_out", "pll_re_vco", 0,
					 clk_base + PLLRE_BASE, 16, 4, 0,
					 pll_re_div_table, &pll_re_lock);
	clks[TEGRA114_CLK_PLL_RE_OUT] = clk;

	/* PLLE */
	clk = tegra_clk_register_plle_tegra114("pll_e_out0", "pll_ref",
				      clk_base, 0, &pll_e_params, NULL);
	clks[TEGRA114_CLK_PLL_E_OUT0] = clk;
}

static const char *mux_audio_sync_clk[] = { "spdif_in_sync", "i2s0_sync",
	"i2s1_sync", "i2s2_sync", "i2s3_sync", "i2s4_sync", "vimclk_sync",
};

static const char *clk_out1_parents[] = { "clk_m", "clk_m_div2",
	"clk_m_div4", "extern1",
};

static const char *clk_out2_parents[] = { "clk_m", "clk_m_div2",
	"clk_m_div4", "extern2",
};

static const char *clk_out3_parents[] = { "clk_m", "clk_m_div2",
	"clk_m_div4", "extern3",
};

static void __init tegra114_audio_clk_init(void __iomem *clk_base)
{
	struct clk *clk;

	/* spdif_in_sync */
	clk = tegra_clk_register_sync_source("spdif_in_sync", 24000000,
					     24000000);
	clks[TEGRA114_CLK_SPDIF_IN_SYNC] = clk;

	/* i2s0_sync */
	clk = tegra_clk_register_sync_source("i2s0_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_I2S0_SYNC] = clk;

	/* i2s1_sync */
	clk = tegra_clk_register_sync_source("i2s1_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_I2S1_SYNC] = clk;

	/* i2s2_sync */
	clk = tegra_clk_register_sync_source("i2s2_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_I2S2_SYNC] = clk;

	/* i2s3_sync */
	clk = tegra_clk_register_sync_source("i2s3_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_I2S3_SYNC] = clk;

	/* i2s4_sync */
	clk = tegra_clk_register_sync_source("i2s4_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_I2S4_SYNC] = clk;

	/* vimclk_sync */
	clk = tegra_clk_register_sync_source("vimclk_sync", 24000000, 24000000);
	clks[TEGRA114_CLK_VIMCLK_SYNC] = clk;

	/* audio0 */
	clk = clk_register_mux(NULL, "audio0_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_I2S0, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_AUDIO0_MUX] = clk;
	clk = clk_register_gate(NULL, "audio0", "audio0_mux", 0,
				clk_base + AUDIO_SYNC_CLK_I2S0, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_AUDIO0] = clk;

	/* audio1 */
	clk = clk_register_mux(NULL, "audio1_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_I2S1, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_AUDIO1_MUX] = clk;
	clk = clk_register_gate(NULL, "audio1", "audio1_mux", 0,
				clk_base + AUDIO_SYNC_CLK_I2S1, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_AUDIO1] = clk;

	/* audio2 */
	clk = clk_register_mux(NULL, "audio2_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_I2S2, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_AUDIO2_MUX] = clk;
	clk = clk_register_gate(NULL, "audio2", "audio2_mux", 0,
				clk_base + AUDIO_SYNC_CLK_I2S2, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_AUDIO2] = clk;

	/* audio3 */
	clk = clk_register_mux(NULL, "audio3_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_I2S3, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_AUDIO3_MUX] = clk;
	clk = clk_register_gate(NULL, "audio3", "audio3_mux", 0,
				clk_base + AUDIO_SYNC_CLK_I2S3, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_AUDIO3] = clk;

	/* audio4 */
	clk = clk_register_mux(NULL, "audio4_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_I2S4, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_AUDIO4_MUX] = clk;
	clk = clk_register_gate(NULL, "audio4", "audio4_mux", 0,
				clk_base + AUDIO_SYNC_CLK_I2S4, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_AUDIO4] = clk;

	/* spdif */
	clk = clk_register_mux(NULL, "spdif_mux", mux_audio_sync_clk,
			       ARRAY_SIZE(mux_audio_sync_clk),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + AUDIO_SYNC_CLK_SPDIF, 0, 3, 0,
			       NULL);
	clks[TEGRA114_CLK_SPDIF_MUX] = clk;
	clk = clk_register_gate(NULL, "spdif", "spdif_mux", 0,
				clk_base + AUDIO_SYNC_CLK_SPDIF, 4,
				CLK_GATE_SET_TO_DISABLE, NULL);
	clks[TEGRA114_CLK_SPDIF] = clk;

	/* audio0_2x */
	clk = clk_register_fixed_factor(NULL, "audio0_doubler", "audio0",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("audio0_div", "audio0_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 24, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("audio0_2x", "audio0_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 113,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_AUDIO0_2X] = clk;

	/* audio1_2x */
	clk = clk_register_fixed_factor(NULL, "audio1_doubler", "audio1",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("audio1_div", "audio1_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 25, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("audio1_2x", "audio1_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 114,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_AUDIO1_2X] = clk;

	/* audio2_2x */
	clk = clk_register_fixed_factor(NULL, "audio2_doubler", "audio2",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("audio2_div", "audio2_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 26, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("audio2_2x", "audio2_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 115,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_AUDIO2_2X] = clk;

	/* audio3_2x */
	clk = clk_register_fixed_factor(NULL, "audio3_doubler", "audio3",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("audio3_div", "audio3_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 27, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("audio3_2x", "audio3_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 116,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_AUDIO3_2X] = clk;

	/* audio4_2x */
	clk = clk_register_fixed_factor(NULL, "audio4_doubler", "audio4",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("audio4_div", "audio4_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 28, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("audio4_2x", "audio4_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 117,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_AUDIO4_2X] = clk;

	/* spdif_2x */
	clk = clk_register_fixed_factor(NULL, "spdif_doubler", "spdif",
					CLK_SET_RATE_PARENT, 2, 1);
	clk = tegra_clk_register_divider("spdif_div", "spdif_doubler",
				clk_base + AUDIO_SYNC_DOUBLER, 0, 0, 29, 1,
				0, &clk_doubler_lock);
	clk = tegra_clk_register_periph_gate("spdif_2x", "spdif_div",
				  TEGRA_PERIPH_NO_RESET, clk_base,
				  CLK_SET_RATE_PARENT, 118,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_SPDIF_2X] = clk;
}

static void __init tegra114_pmc_clk_init(void __iomem *pmc_base)
{
	struct clk *clk;

	/* clk_out_1 */
	clk = clk_register_mux(NULL, "clk_out_1_mux", clk_out1_parents,
			       ARRAY_SIZE(clk_out1_parents),
			       CLK_SET_RATE_NO_REPARENT,
			       pmc_base + PMC_CLK_OUT_CNTRL, 6, 3, 0,
			       &clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_1_MUX] = clk;
	clk = clk_register_gate(NULL, "clk_out_1", "clk_out_1_mux", 0,
				pmc_base + PMC_CLK_OUT_CNTRL, 2, 0,
				&clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_1] = clk;

	/* clk_out_2 */
	clk = clk_register_mux(NULL, "clk_out_2_mux", clk_out2_parents,
			       ARRAY_SIZE(clk_out2_parents),
			       CLK_SET_RATE_NO_REPARENT,
			       pmc_base + PMC_CLK_OUT_CNTRL, 14, 3, 0,
			       &clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_2_MUX] = clk;
	clk = clk_register_gate(NULL, "clk_out_2", "clk_out_2_mux", 0,
				pmc_base + PMC_CLK_OUT_CNTRL, 10, 0,
				&clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_2] = clk;

	/* clk_out_3 */
	clk = clk_register_mux(NULL, "clk_out_3_mux", clk_out3_parents,
			       ARRAY_SIZE(clk_out3_parents),
			       CLK_SET_RATE_NO_REPARENT,
			       pmc_base + PMC_CLK_OUT_CNTRL, 22, 3, 0,
			       &clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_3_MUX] = clk;
	clk = clk_register_gate(NULL, "clk_out_3", "clk_out_3_mux", 0,
				pmc_base + PMC_CLK_OUT_CNTRL, 18, 0,
				&clk_out_lock);
	clks[TEGRA114_CLK_CLK_OUT_3] = clk;

	/* blink */
	/* clear the blink timer register to directly output clk_32k */
	writel_relaxed(0, pmc_base + PMC_BLINK_TIMER);
	clk = clk_register_gate(NULL, "blink_override", "clk_32k", 0,
				pmc_base + PMC_DPD_PADS_ORIDE,
				PMC_DPD_PADS_ORIDE_BLINK_ENB, 0, NULL);
	clk = clk_register_gate(NULL, "blink", "blink_override", 0,
				pmc_base + PMC_CTRL,
				PMC_CTRL_BLINK_ENB, 0, NULL);
	clks[TEGRA114_CLK_BLINK] = clk;

}

static const char *sclk_parents[] = { "clk_m", "pll_c_out1", "pll_p_out4",
			       "pll_p", "pll_p_out2", "unused",
			       "clk_32k", "pll_m_out1" };

static const char *cclk_g_parents[] = { "clk_m", "pll_c", "clk_32k", "pll_m",
					"pll_p", "pll_p_out4", "unused",
					"unused", "pll_x" };

static const char *cclk_lp_parents[] = { "clk_m", "pll_c", "clk_32k", "pll_m",
					 "pll_p", "pll_p_out4", "unused",
					 "unused", "pll_x", "pll_x_out0" };

static void __init tegra114_super_clk_init(void __iomem *clk_base)
{
	struct clk *clk;

	/* CCLKG */
	clk = tegra_clk_register_super_mux("cclk_g", cclk_g_parents,
					ARRAY_SIZE(cclk_g_parents),
					CLK_SET_RATE_PARENT,
					clk_base + CCLKG_BURST_POLICY,
					0, 4, 0, 0, NULL);
	clks[TEGRA114_CLK_CCLK_G] = clk;

	/* CCLKLP */
	clk = tegra_clk_register_super_mux("cclk_lp", cclk_lp_parents,
					ARRAY_SIZE(cclk_lp_parents),
					CLK_SET_RATE_PARENT,
					clk_base + CCLKLP_BURST_POLICY,
					0, 4, 8, 9, NULL);
	clks[TEGRA114_CLK_CCLK_LP] = clk;

	/* SCLK */
	clk = tegra_clk_register_super_mux("sclk", sclk_parents,
					ARRAY_SIZE(sclk_parents),
					CLK_SET_RATE_PARENT,
					clk_base + SCLK_BURST_POLICY,
					0, 4, 0, 0, NULL);
	clks[TEGRA114_CLK_SCLK] = clk;

	/* HCLK */
	clk = clk_register_divider(NULL, "hclk_div", "sclk", 0,
				   clk_base + SYSTEM_CLK_RATE, 4, 2, 0,
				   &sysrate_lock);
	clk = clk_register_gate(NULL, "hclk", "hclk_div", CLK_SET_RATE_PARENT |
				CLK_IGNORE_UNUSED, clk_base + SYSTEM_CLK_RATE,
				7, CLK_GATE_SET_TO_DISABLE, &sysrate_lock);
	clks[TEGRA114_CLK_HCLK] = clk;

	/* PCLK */
	clk = clk_register_divider(NULL, "pclk_div", "hclk", 0,
				   clk_base + SYSTEM_CLK_RATE, 0, 2, 0,
				   &sysrate_lock);
	clk = clk_register_gate(NULL, "pclk", "pclk_div", CLK_SET_RATE_PARENT |
				CLK_IGNORE_UNUSED, clk_base + SYSTEM_CLK_RATE,
				3, CLK_GATE_SET_TO_DISABLE, &sysrate_lock);
	clks[TEGRA114_CLK_PCLK] = clk;
}

static struct tegra_periph_init_data tegra_periph_clk_list[] = {
	TEGRA_INIT_DATA_MUX("i2s0", mux_pllaout0_audio0_2x_pllp_clkm, CLK_SOURCE_I2S0, 30, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2S0),
	TEGRA_INIT_DATA_MUX("i2s1", mux_pllaout0_audio1_2x_pllp_clkm, CLK_SOURCE_I2S1, 11, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2S1),
	TEGRA_INIT_DATA_MUX("i2s2", mux_pllaout0_audio2_2x_pllp_clkm, CLK_SOURCE_I2S2, 18, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2S2),
	TEGRA_INIT_DATA_MUX("i2s3", mux_pllaout0_audio3_2x_pllp_clkm, CLK_SOURCE_I2S3, 101, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2S3),
	TEGRA_INIT_DATA_MUX("i2s4", mux_pllaout0_audio4_2x_pllp_clkm, CLK_SOURCE_I2S4, 102, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2S4),
	TEGRA_INIT_DATA_MUX("spdif_out", mux_pllaout0_audio_2x_pllp_clkm, CLK_SOURCE_SPDIF_OUT, 10, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SPDIF_OUT),
	TEGRA_INIT_DATA_MUX("spdif_in", mux_pllp_pllc_pllm, CLK_SOURCE_SPDIF_IN, 10, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SPDIF_IN),
	TEGRA_INIT_DATA_MUX("pwm", mux_pllp_pllc_clk32_clkm, CLK_SOURCE_PWM, 17, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_PWM),
	TEGRA_INIT_DATA_MUX("adx", mux_plla_pllc_pllp_clkm, CLK_SOURCE_ADX, 154, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_ADX),
	TEGRA_INIT_DATA_MUX("amx", mux_plla_pllc_pllp_clkm, CLK_SOURCE_AMX, 153, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_AMX),
	TEGRA_INIT_DATA_MUX("hda", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_HDA, 125, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_HDA),
	TEGRA_INIT_DATA_MUX("hda2codec_2x", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_HDA2CODEC_2X, 111, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_HDA2CODEC_2X),
	TEGRA_INIT_DATA_MUX8("sbc1", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC1, 41, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC1),
	TEGRA_INIT_DATA_MUX8("sbc2", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC2, 44, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC2),
	TEGRA_INIT_DATA_MUX8("sbc3", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC3, 46, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC3),
	TEGRA_INIT_DATA_MUX8("sbc4", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC4, 68, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC4),
	TEGRA_INIT_DATA_MUX8("sbc5", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC5, 104, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC5),
	TEGRA_INIT_DATA_MUX8("sbc6", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SBC6, 105, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SBC6),
	TEGRA_INIT_DATA_MUX8("ndflash", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_NDFLASH, 13, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_NDSPEED),
	TEGRA_INIT_DATA_MUX8("ndspeed", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_NDSPEED, 80, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_NDSPEED),
	TEGRA_INIT_DATA_MUX("vfir", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_VFIR, 7, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_VFIR),
	TEGRA_INIT_DATA_MUX("sdmmc1", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_SDMMC1, 14, 0, TEGRA114_CLK_SDMMC1),
	TEGRA_INIT_DATA_MUX("sdmmc2", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_SDMMC2, 9, 0, TEGRA114_CLK_SDMMC2),
	TEGRA_INIT_DATA_MUX("sdmmc3", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_SDMMC3, 69, 0, TEGRA114_CLK_SDMMC3),
	TEGRA_INIT_DATA_MUX("sdmmc4", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_SDMMC4, 15, 0, TEGRA114_CLK_SDMMC4),
	TEGRA_INIT_DATA_INT8("vde", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_VDE, 61, 0, TEGRA114_CLK_VDE),
	TEGRA_INIT_DATA_MUX_FLAGS("csite", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_CSITE, 73, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_CSITE, CLK_IGNORE_UNUSED),
	TEGRA_INIT_DATA_MUX("la", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_LA, 76, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_LA),
	TEGRA_INIT_DATA_MUX("trace", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_TRACE, 77, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_TRACE),
	TEGRA_INIT_DATA_MUX("owr", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_OWR, 71, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_OWR),
	TEGRA_INIT_DATA_MUX("nor", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_NOR, 42, 0, TEGRA114_CLK_NOR),
	TEGRA_INIT_DATA_MUX("mipi", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_MIPI, 50, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_MIPI),
	TEGRA_INIT_DATA_I2C("i2c1", mux_pllp_clkm, CLK_SOURCE_I2C1, 12, TEGRA114_CLK_I2C1),
	TEGRA_INIT_DATA_I2C("i2c2", mux_pllp_clkm, CLK_SOURCE_I2C2, 54, TEGRA114_CLK_I2C2),
	TEGRA_INIT_DATA_I2C("i2c3", mux_pllp_clkm, CLK_SOURCE_I2C3, 67, TEGRA114_CLK_I2C3),
	TEGRA_INIT_DATA_I2C("i2c4", mux_pllp_clkm, CLK_SOURCE_I2C4, 103, TEGRA114_CLK_I2C4),
	TEGRA_INIT_DATA_I2C("i2c5", mux_pllp_clkm, CLK_SOURCE_I2C5, 47, TEGRA114_CLK_I2C5),
	TEGRA_INIT_DATA_UART("uarta", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_UARTA, 6, TEGRA114_CLK_UARTA),
	TEGRA_INIT_DATA_UART("uartb", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_UARTB, 7, TEGRA114_CLK_UARTB),
	TEGRA_INIT_DATA_UART("uartc", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_UARTC, 55, TEGRA114_CLK_UARTC),
	TEGRA_INIT_DATA_UART("uartd", mux_pllp_pllc_pllm_clkm, CLK_SOURCE_UARTD, 65, TEGRA114_CLK_UARTD),
	TEGRA_INIT_DATA_INT8("3d", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_3D, 24, 0, TEGRA114_CLK_GR3D),
	TEGRA_INIT_DATA_INT8("2d", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_2D, 21, 0, TEGRA114_CLK_GR2D),
	TEGRA_INIT_DATA_MUX("vi_sensor", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_VI_SENSOR, 20, TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_VI_SENSOR),
	TEGRA_INIT_DATA_INT8("vi", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_VI, 20, 0, TEGRA114_CLK_VI),
	TEGRA_INIT_DATA_INT8("epp", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_EPP, 19, 0, TEGRA114_CLK_EPP),
	TEGRA_INIT_DATA_INT8("msenc", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_MSENC, 91, TEGRA_PERIPH_WAR_1005168, TEGRA114_CLK_MSENC),
	TEGRA_INIT_DATA_INT8("tsec", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_TSEC, 83, 0, TEGRA114_CLK_TSEC),
	TEGRA_INIT_DATA_INT8("host1x", mux_pllm_pllc2_c_c3_pllp_plla, CLK_SOURCE_HOST1X, 28, 0, TEGRA114_CLK_HOST1X),
	TEGRA_INIT_DATA_MUX8("hdmi", mux_pllp_pllm_plld_plla_pllc_plld2_clkm, CLK_SOURCE_HDMI, 51, 0, TEGRA114_CLK_HDMI),
	TEGRA_INIT_DATA_MUX("cilab", mux_pllp_pllc_clkm, CLK_SOURCE_CILAB, 144, 0, TEGRA114_CLK_CILAB),
	TEGRA_INIT_DATA_MUX("cilcd", mux_pllp_pllc_clkm, CLK_SOURCE_CILCD, 145, 0, TEGRA114_CLK_CILCD),
	TEGRA_INIT_DATA_MUX("cile", mux_pllp_pllc_clkm, CLK_SOURCE_CILE, 146, 0, TEGRA114_CLK_CILE),
	TEGRA_INIT_DATA_MUX("dsialp", mux_pllp_pllc_clkm, CLK_SOURCE_DSIALP, 147, 0, TEGRA114_CLK_DSIALP),
	TEGRA_INIT_DATA_MUX("dsiblp", mux_pllp_pllc_clkm, CLK_SOURCE_DSIBLP, 148, 0, TEGRA114_CLK_DSIBLP),
	TEGRA_INIT_DATA_MUX("tsensor", mux_pllp_pllc_clkm_clk32, CLK_SOURCE_TSENSOR, 100, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_TSENSOR),
	TEGRA_INIT_DATA_MUX("actmon", mux_pllp_pllc_clk32_clkm, CLK_SOURCE_ACTMON, 119, 0, TEGRA114_CLK_ACTMON),
	TEGRA_INIT_DATA_MUX8("extern1", mux_plla_clk32_pllp_clkm_plle, CLK_SOURCE_EXTERN1, 120, 0, TEGRA114_CLK_EXTERN1),
	TEGRA_INIT_DATA_MUX8("extern2", mux_plla_clk32_pllp_clkm_plle, CLK_SOURCE_EXTERN2, 121, 0, TEGRA114_CLK_EXTERN2),
	TEGRA_INIT_DATA_MUX8("extern3", mux_plla_clk32_pllp_clkm_plle, CLK_SOURCE_EXTERN3, 122, 0, TEGRA114_CLK_EXTERN3),
	TEGRA_INIT_DATA_MUX("i2cslow", mux_pllp_pllc_clk32_clkm, CLK_SOURCE_I2CSLOW, 81, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_I2CSLOW),
	TEGRA_INIT_DATA_INT8("se", mux_pllp_pllc2_c_c3_pllm_clkm, CLK_SOURCE_SE, 127, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SE),
	TEGRA_INIT_DATA_INT_FLAGS("mselect", mux_pllp_clkm, CLK_SOURCE_MSELECT, 99, 0, TEGRA114_CLK_MSELECT, CLK_IGNORE_UNUSED),
	TEGRA_INIT_DATA_MUX("dfll_ref", mux_pllp_clkm, CLK_SOURCE_DFLL_REF, 155, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_DFLL_REF),
	TEGRA_INIT_DATA_MUX("dfll_soc", mux_pllp_clkm, CLK_SOURCE_DFLL_SOC, 155, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_DFLL_SOC),
	TEGRA_INIT_DATA_MUX8("soc_therm", mux_pllm_pllc_pllp_plla, CLK_SOURCE_SOC_THERM, 78, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_SOC_THERM),
	TEGRA_INIT_DATA_XUSB("xusb_host_src", mux_clkm_pllp_pllc_pllre, CLK_SOURCE_XUSB_HOST_SRC, 143, TEGRA_PERIPH_ON_APB | TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_XUSB_HOST_SRC),
	TEGRA_INIT_DATA_XUSB("xusb_falcon_src", mux_clkm_pllp_pllc_pllre, CLK_SOURCE_XUSB_FALCON_SRC, 143, TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_XUSB_FALCON_SRC),
	TEGRA_INIT_DATA_XUSB("xusb_fs_src", mux_clkm_48M_pllp_480M, CLK_SOURCE_XUSB_FS_SRC, 143, TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_XUSB_FS_SRC),
	TEGRA_INIT_DATA_XUSB("xusb_ss_src", mux_clkm_pllre_clk32_480M_pllc_ref, CLK_SOURCE_XUSB_SS_SRC, 143, TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_XUSB_SS_SRC),
	TEGRA_INIT_DATA_XUSB("xusb_dev_src", mux_clkm_pllp_pllc_pllre, CLK_SOURCE_XUSB_DEV_SRC, 95, TEGRA_PERIPH_ON_APB | TEGRA_PERIPH_NO_RESET, TEGRA114_CLK_XUSB_DEV_SRC),
	TEGRA_INIT_DATA_AUDIO("d_audio", CLK_SOURCE_D_AUDIO, 106, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_D_AUDIO),
	TEGRA_INIT_DATA_AUDIO("dam0", CLK_SOURCE_DAM0, 108, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_DAM0),
	TEGRA_INIT_DATA_AUDIO("dam1", CLK_SOURCE_DAM1, 109, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_DAM1),
	TEGRA_INIT_DATA_AUDIO("dam2", CLK_SOURCE_DAM2, 110, TEGRA_PERIPH_ON_APB, TEGRA114_CLK_DAM2),
};

static struct tegra_periph_init_data tegra_periph_nodiv_clk_list[] = {
	TEGRA_INIT_DATA_NODIV("disp1", mux_pllp_pllm_plld_plla_pllc_plld2_clkm, CLK_SOURCE_DISP1, 29, 7, 27, 0, TEGRA114_CLK_DISP1),
	TEGRA_INIT_DATA_NODIV("disp2", mux_pllp_pllm_plld_plla_pllc_plld2_clkm, CLK_SOURCE_DISP2, 29, 7, 26, 0, TEGRA114_CLK_DISP2),
};

static __init void tegra114_periph_clk_init(void __iomem *clk_base)
{
	struct tegra_periph_init_data *data;
	struct clk *clk;
	int i;
	u32 val;

	/* apbdma */
	clk = tegra_clk_register_periph_gate("apbdma", "clk_m", 0, clk_base,
				  0, 34, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_APBDMA] = clk;

	/* rtc */
	clk = tegra_clk_register_periph_gate("rtc", "clk_32k",
				    TEGRA_PERIPH_ON_APB |
				    TEGRA_PERIPH_NO_RESET, clk_base,
				    0, 4, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_RTC] = clk;

	/* kbc */
	clk = tegra_clk_register_periph_gate("kbc", "clk_32k",
				    TEGRA_PERIPH_ON_APB |
				    TEGRA_PERIPH_NO_RESET, clk_base,
				    0, 36, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_KBC] = clk;

	/* timer */
	clk = tegra_clk_register_periph_gate("timer", "clk_m", 0, clk_base,
				  0, 5, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_TIMER] = clk;

	/* kfuse */
	clk = tegra_clk_register_periph_gate("kfuse", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base,  0, 40,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_KFUSE] = clk;

	/* fuse */
	clk = tegra_clk_register_periph_gate("fuse", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base,  0, 39,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_FUSE] = clk;

	/* fuse_burn */
	clk = tegra_clk_register_periph_gate("fuse_burn", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base,  0, 39,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_FUSE_BURN] = clk;

	/* apbif */
	clk = tegra_clk_register_periph_gate("apbif", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base,  0, 107,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_APBIF] = clk;

	/* hda2hdmi */
	clk = tegra_clk_register_periph_gate("hda2hdmi", "clk_m",
				    TEGRA_PERIPH_ON_APB, clk_base,  0, 128,
				    periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_HDA2HDMI] = clk;

	/* vcp */
	clk = tegra_clk_register_periph_gate("vcp", "clk_m", 0, clk_base,  0,
				  29, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_VCP] = clk;

	/* bsea */
	clk = tegra_clk_register_periph_gate("bsea", "clk_m", 0, clk_base,
				  0, 62, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_BSEA] = clk;

	/* bsev */
	clk = tegra_clk_register_periph_gate("bsev", "clk_m", 0, clk_base,
				  0, 63, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_BSEV] = clk;

	/* mipi-cal */
	clk = tegra_clk_register_periph_gate("mipi-cal", "clk_m", 0, clk_base,
				   0, 56, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_MIPI_CAL] = clk;

	/* usbd */
	clk = tegra_clk_register_periph_gate("usbd", "clk_m", 0, clk_base,
				  0, 22, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_USBD] = clk;

	/* usb2 */
	clk = tegra_clk_register_periph_gate("usb2", "clk_m", 0, clk_base,
				  0, 58, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_USB2] = clk;

	/* usb3 */
	clk = tegra_clk_register_periph_gate("usb3", "clk_m", 0, clk_base,
				  0, 59, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_USB3] = clk;

	/* csi */
	clk = tegra_clk_register_periph_gate("csi", "pll_p_out3", 0, clk_base,
				   0, 52, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_CSI] = clk;

	/* isp */
	clk = tegra_clk_register_periph_gate("isp", "clk_m", 0, clk_base, 0,
				  23, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_ISP] = clk;

	/* csus */
	clk = tegra_clk_register_periph_gate("csus", "clk_m",
				  TEGRA_PERIPH_NO_RESET, clk_base, 0, 92,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_CSUS] = clk;

	/* dds */
	clk = tegra_clk_register_periph_gate("dds", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base, 0, 150,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_DDS] = clk;

	/* dp2 */
	clk = tegra_clk_register_periph_gate("dp2", "clk_m",
				  TEGRA_PERIPH_ON_APB, clk_base, 0, 152,
				  periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_DP2] = clk;

	/* dtv */
	clk = tegra_clk_register_periph_gate("dtv", "clk_m",
				    TEGRA_PERIPH_ON_APB, clk_base, 0, 79,
				    periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_DTV] = clk;

	/* dsia */
	clk = clk_register_mux(NULL, "dsia_mux", mux_plld_out0_plld2_out0,
			       ARRAY_SIZE(mux_plld_out0_plld2_out0),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + PLLD_BASE, 25, 1, 0, &pll_d_lock);
	clks[TEGRA114_CLK_DSIA_MUX] = clk;
	clk = tegra_clk_register_periph_gate("dsia", "dsia_mux", 0, clk_base,
				    0, 48, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_DSIA] = clk;

	/* dsib */
	clk = clk_register_mux(NULL, "dsib_mux", mux_plld_out0_plld2_out0,
			       ARRAY_SIZE(mux_plld_out0_plld2_out0),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + PLLD2_BASE, 25, 1, 0, &pll_d2_lock);
	clks[TEGRA114_CLK_DSIB_MUX] = clk;
	clk = tegra_clk_register_periph_gate("dsib", "dsib_mux", 0, clk_base,
				    0, 82, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_DSIB] = clk;

	/* xusb_hs_src */
	val = readl(clk_base + CLK_SOURCE_XUSB_SS_SRC);
	val |= BIT(25); /* always select PLLU_60M */
	writel(val, clk_base + CLK_SOURCE_XUSB_SS_SRC);

	clk = clk_register_fixed_factor(NULL, "xusb_hs_src", "pll_u_60M", 0,
					1, 1);
	clks[TEGRA114_CLK_XUSB_HS_SRC] = clk;

	/* xusb_host */
	clk = tegra_clk_register_periph_gate("xusb_host", "xusb_host_src", 0,
				    clk_base, 0, 89, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_XUSB_HOST] = clk;

	/* xusb_ss */
	clk = tegra_clk_register_periph_gate("xusb_ss", "xusb_ss_src", 0,
				    clk_base, 0, 156, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_XUSB_HOST] = clk;

	/* xusb_dev */
	clk = tegra_clk_register_periph_gate("xusb_dev", "xusb_dev_src", 0,
				    clk_base, 0, 95, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_XUSB_DEV] = clk;

	/* emc */
	clk = clk_register_mux(NULL, "emc_mux", mux_pllmcp_clkm,
			       ARRAY_SIZE(mux_pllmcp_clkm),
			       CLK_SET_RATE_NO_REPARENT,
			       clk_base + CLK_SOURCE_EMC,
			       29, 3, 0, NULL);
	clk = tegra_clk_register_periph_gate("emc", "emc_mux", 0, clk_base,
				CLK_IGNORE_UNUSED, 57, periph_clk_enb_refcnt);
	clks[TEGRA114_CLK_EMC] = clk;

	for (i = 0; i < ARRAY_SIZE(tegra_periph_clk_list); i++) {
		data = &tegra_periph_clk_list[i];

		clk = tegra_clk_register_periph(data->name,
			data->parent_names, data->num_parents, &data->periph,
			clk_base, data->offset, data->flags);
		clks[data->clk_id] = clk;
	}

	for (i = 0; i < ARRAY_SIZE(tegra_periph_nodiv_clk_list); i++) {
		data = &tegra_periph_nodiv_clk_list[i];

		clk = tegra_clk_register_periph_nodiv(data->name,
				data->parent_names, data->num_parents,
				&data->periph, clk_base, data->offset);
		clks[data->clk_id] = clk;
	}
}

/* Tegra114 CPU clock and reset control functions */
static void tegra114_wait_cpu_in_reset(u32 cpu)
{
	unsigned int reg;

	do {
		reg = readl(clk_base + CLK_RST_CONTROLLER_CPU_CMPLX_STATUS);
		cpu_relax();
	} while (!(reg & (1 << cpu)));  /* check CPU been reset or not */
}
static void tegra114_disable_cpu_clock(u32 cpu)
{
	/* flow controller would take care in the power sequence. */
}

#ifdef CONFIG_PM_SLEEP
static void tegra114_cpu_clock_suspend(void)
{
	/* switch coresite to clk_m, save off original source */
	tegra114_cpu_clk_sctx.clk_csite_src =
				readl(clk_base + CLK_SOURCE_CSITE);
	writel(3 << 30, clk_base + CLK_SOURCE_CSITE);

	tegra114_cpu_clk_sctx.cclkg_burst =
				readl(clk_base + CCLKG_BURST_POLICY);
	tegra114_cpu_clk_sctx.cclkg_divider =
				readl(clk_base + CCLKG_BURST_POLICY + 4);
}

static void tegra114_cpu_clock_resume(void)
{
	writel(tegra114_cpu_clk_sctx.clk_csite_src,
					clk_base + CLK_SOURCE_CSITE);

	writel(tegra114_cpu_clk_sctx.cclkg_burst,
					clk_base + CCLKG_BURST_POLICY);
	writel(tegra114_cpu_clk_sctx.cclkg_divider,
					clk_base + CCLKG_BURST_POLICY + 4);
}
#endif

static struct tegra_cpu_car_ops tegra114_cpu_car_ops = {
	.wait_for_reset	= tegra114_wait_cpu_in_reset,
	.disable_clock	= tegra114_disable_cpu_clock,
#ifdef CONFIG_PM_SLEEP
	.suspend	= tegra114_cpu_clock_suspend,
	.resume		= tegra114_cpu_clock_resume,
#endif
};

static const struct of_device_id pmc_match[] __initconst = {
	{ .compatible = "nvidia,tegra114-pmc" },
	{},
};

/*
 * dfll_soc/dfll_ref apparently must be kept enabled, otherwise I2C5
 * breaks
 */
static struct tegra_clk_init_table init_table[] __initdata = {
	{TEGRA114_CLK_UARTA, TEGRA114_CLK_PLL_P, 408000000, 0},
	{TEGRA114_CLK_UARTB, TEGRA114_CLK_PLL_P, 408000000, 0},
	{TEGRA114_CLK_UARTC, TEGRA114_CLK_PLL_P, 408000000, 0},
	{TEGRA114_CLK_UARTD, TEGRA114_CLK_PLL_P, 408000000, 0},
	{TEGRA114_CLK_PLL_A, TEGRA114_CLK_CLK_MAX, 564480000, 1},
	{TEGRA114_CLK_PLL_A_OUT0, TEGRA114_CLK_CLK_MAX, 11289600, 1},
	{TEGRA114_CLK_EXTERN1, TEGRA114_CLK_PLL_A_OUT0, 0, 1},
	{TEGRA114_CLK_CLK_OUT_1_MUX, TEGRA114_CLK_EXTERN1, 0, 1},
	{TEGRA114_CLK_CLK_OUT_1, TEGRA114_CLK_CLK_MAX, 0, 1},
	{TEGRA114_CLK_I2S0, TEGRA114_CLK_PLL_A_OUT0, 11289600, 0},
	{TEGRA114_CLK_I2S1, TEGRA114_CLK_PLL_A_OUT0, 11289600, 0},
	{TEGRA114_CLK_I2S2, TEGRA114_CLK_PLL_A_OUT0, 11289600, 0},
	{TEGRA114_CLK_I2S3, TEGRA114_CLK_PLL_A_OUT0, 11289600, 0},
	{TEGRA114_CLK_I2S4, TEGRA114_CLK_PLL_A_OUT0, 11289600, 0},
	{TEGRA114_CLK_HOST1X, TEGRA114_CLK_PLL_P, 136000000, 0},
	{TEGRA114_CLK_DFLL_SOC, TEGRA114_CLK_PLL_P, 51000000, 1},
	{TEGRA114_CLK_DFLL_REF, TEGRA114_CLK_PLL_P, 51000000, 1},
	{TEGRA114_CLK_GR2D, TEGRA114_CLK_PLL_C2, 300000000, 0},
	{TEGRA114_CLK_GR3D, TEGRA114_CLK_PLL_C2, 300000000, 0},

	/* This MUST be the last entry. */
	{TEGRA114_CLK_CLK_MAX, TEGRA114_CLK_CLK_MAX, 0, 0},
};

static void __init tegra114_clock_apply_init_table(void)
{
	tegra_init_from_table(init_table, clks, TEGRA114_CLK_CLK_MAX);
}


/**
 * tegra114_car_barrier - wait for pending writes to the CAR to complete
 *
 * Wait for any outstanding writes to the CAR MMIO space from this CPU
 * to complete before continuing execution.  No return value.
 */
static void tegra114_car_barrier(void)
{
	wmb();		/* probably unnecessary */
	readl_relaxed(clk_base + CPU_FINETRIM_SELECT);
}

/**
 * tegra114_clock_tune_cpu_trimmers_high - use high-voltage propagation delays
 *
 * When the CPU rail voltage is in the high-voltage range, use the
 * built-in hardwired clock propagation delays in the CPU clock
 * shaper.  No return value.
 */
void tegra114_clock_tune_cpu_trimmers_high(void)
{
	u32 select = 0;

	/* Use hardwired rise->rise & fall->fall clock propagation delays */
	select |= ~(CPU_FINETRIM_1_FCPU_1 | CPU_FINETRIM_1_FCPU_2 |
		    CPU_FINETRIM_1_FCPU_3 | CPU_FINETRIM_1_FCPU_4 |
		    CPU_FINETRIM_1_FCPU_5 | CPU_FINETRIM_1_FCPU_6);
	writel_relaxed(select, clk_base + CPU_FINETRIM_SELECT);

	tegra114_car_barrier();
}
EXPORT_SYMBOL(tegra114_clock_tune_cpu_trimmers_high);

/**
 * tegra114_clock_tune_cpu_trimmers_low - use low-voltage propagation delays
 *
 * When the CPU rail voltage is in the low-voltage range, use the
 * extended clock propagation delays set by
 * tegra114_clock_tune_cpu_trimmers_init().  The intention is to
 * maintain the input clock duty cycle that the FCPU subsystem
 * expects.  No return value.
 */
void tegra114_clock_tune_cpu_trimmers_low(void)
{
	u32 select = 0;

	/*
	 * Use software-specified rise->rise & fall->fall clock
	 * propagation delays (from
	 * tegra114_clock_tune_cpu_trimmers_init()
	 */
	select |= (CPU_FINETRIM_1_FCPU_1 | CPU_FINETRIM_1_FCPU_2 |
		   CPU_FINETRIM_1_FCPU_3 | CPU_FINETRIM_1_FCPU_4 |
		   CPU_FINETRIM_1_FCPU_5 | CPU_FINETRIM_1_FCPU_6);
	writel_relaxed(select, clk_base + CPU_FINETRIM_SELECT);

	tegra114_car_barrier();
}
EXPORT_SYMBOL(tegra114_clock_tune_cpu_trimmers_low);

/**
 * tegra114_clock_tune_cpu_trimmers_init - set up and enable clk prop delays
 *
 * Program extended clock propagation delays into the FCPU clock
 * shaper and enable them.  XXX Define the purpose - peak current
 * reduction?  No return value.
 */
/* XXX Initial voltage rail state assumption issues? */
void tegra114_clock_tune_cpu_trimmers_init(void)
{
	u32 dr = 0, r = 0;

	/* Increment the rise->rise clock delay by four steps */
	r |= (CPU_FINETRIM_R_FCPU_1_MASK | CPU_FINETRIM_R_FCPU_2_MASK |
	      CPU_FINETRIM_R_FCPU_3_MASK | CPU_FINETRIM_R_FCPU_4_MASK |
	      CPU_FINETRIM_R_FCPU_5_MASK | CPU_FINETRIM_R_FCPU_6_MASK);
	writel_relaxed(r, clk_base + CPU_FINETRIM_R);

	/*
	 * Use the rise->rise clock propagation delay specified in the
	 * r field
	 */
	dr |= (CPU_FINETRIM_1_FCPU_1 | CPU_FINETRIM_1_FCPU_2 |
	       CPU_FINETRIM_1_FCPU_3 | CPU_FINETRIM_1_FCPU_4 |
	       CPU_FINETRIM_1_FCPU_5 | CPU_FINETRIM_1_FCPU_6);
	writel_relaxed(dr, clk_base + CPU_FINETRIM_DR);

	tegra114_clock_tune_cpu_trimmers_low();
}
EXPORT_SYMBOL(tegra114_clock_tune_cpu_trimmers_init);

/**
 * tegra114_clock_assert_dfll_dvco_reset - assert the DFLL's DVCO reset
 *
 * Assert the reset line of the DFLL's DVCO.  No return value.
 */
void tegra114_clock_assert_dfll_dvco_reset(void)
{
	u32 v;

	v = readl_relaxed(clk_base + RST_DFLL_DVCO);
	v |= (1 << DVFS_DFLL_RESET_SHIFT);
	writel_relaxed(v, clk_base + RST_DFLL_DVCO);
	tegra114_car_barrier();
}
EXPORT_SYMBOL(tegra114_clock_assert_dfll_dvco_reset);

/**
 * tegra114_clock_deassert_dfll_dvco_reset - deassert the DFLL's DVCO reset
 *
 * Deassert the reset line of the DFLL's DVCO, allowing the DVCO to
 * operate.  No return value.
 */
void tegra114_clock_deassert_dfll_dvco_reset(void)
{
	u32 v;

	v = readl_relaxed(clk_base + RST_DFLL_DVCO);
	v &= ~(1 << DVFS_DFLL_RESET_SHIFT);
	writel_relaxed(v, clk_base + RST_DFLL_DVCO);
	tegra114_car_barrier();
}
EXPORT_SYMBOL(tegra114_clock_deassert_dfll_dvco_reset);

static void __init tegra114_clock_init(struct device_node *np)
{
	struct device_node *node;

	clk_base = of_iomap(np, 0);
	if (!clk_base) {
		pr_err("ioremap tegra114 CAR failed\n");
		return;
	}

	node = of_find_matching_node(NULL, pmc_match);
	if (!node) {
		pr_err("Failed to find pmc node\n");
		WARN_ON(1);
		return;
	}

	pmc_base = of_iomap(node, 0);
	if (!pmc_base) {
		pr_err("Can't map pmc registers\n");
		WARN_ON(1);
		return;
	}

	clks = tegra_clk_init(TEGRA114_CLK_CLK_MAX, TEGRA114_CLK_PERIPH_BANKS);
	if (!clks)
		return;

	if (tegra114_osc_clk_init(clk_base) < 0)
		return;

	tegra114_fixed_clk_init(clk_base);
	tegra114_pll_init(clk_base, pmc_base);
	tegra114_periph_clk_init(clk_base);
	tegra114_audio_clk_init(clk_base);
	tegra114_pmc_clk_init(pmc_base);
	tegra114_super_clk_init(clk_base);

	tegra_add_of_provider(np);
	tegra_register_devclks(devclks, ARRAY_SIZE(devclks));

	tegra_clk_apply_init_table = tegra114_clock_apply_init_table;

	tegra_cpu_car_ops = &tegra114_cpu_car_ops;
}
CLK_OF_DECLARE(tegra114, "nvidia,tegra114-car", tegra114_clock_init);
