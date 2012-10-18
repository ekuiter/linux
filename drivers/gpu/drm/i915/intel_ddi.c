/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eugeni Dodonov <eugeni.dodonov@intel.com>
 *
 */

#include "i915_drv.h"
#include "intel_drv.h"

/* HDMI/DVI modes ignore everything but the last 2 items. So we share
 * them for both DP and FDI transports, allowing those ports to
 * automatically adapt to HDMI connections as well
 */
static const u32 hsw_ddi_translations_dp[] = {
	0x00FFFFFF, 0x0006000E,		/* DP parameters */
	0x00D75FFF, 0x0005000A,
	0x00C30FFF, 0x00040006,
	0x80AAAFFF, 0x000B0000,
	0x00FFFFFF, 0x0005000A,
	0x00D75FFF, 0x000C0004,
	0x80C30FFF, 0x000B0000,
	0x00FFFFFF, 0x00040006,
	0x80D75FFF, 0x000B0000,
	0x00FFFFFF, 0x00040006		/* HDMI parameters */
};

static const u32 hsw_ddi_translations_fdi[] = {
	0x00FFFFFF, 0x0007000E,		/* FDI parameters */
	0x00D75FFF, 0x000F000A,
	0x00C30FFF, 0x00060006,
	0x00AAAFFF, 0x001E0000,
	0x00FFFFFF, 0x000F000A,
	0x00D75FFF, 0x00160004,
	0x00C30FFF, 0x001E0000,
	0x00FFFFFF, 0x00060006,
	0x00D75FFF, 0x001E0000,
	0x00FFFFFF, 0x00040006		/* HDMI parameters */
};

static enum port intel_ddi_get_encoder_port(struct intel_encoder *intel_encoder)
{
	struct drm_encoder *encoder = &intel_encoder->base;
	int type = intel_encoder->type;

	if (type == INTEL_OUTPUT_DISPLAYPORT || type == INTEL_OUTPUT_EDP) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
		return intel_dp->port;

	} else if (type == INTEL_OUTPUT_HDMI) {
		struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
		return intel_hdmi->ddi_port;

	} else if (type == INTEL_OUTPUT_ANALOG) {
		return PORT_E;

	} else {
		DRM_ERROR("Invalid DDI encoder type %d\n", type);
		BUG();
	}
}

/* On Haswell, DDI port buffers must be programmed with correct values
 * in advance. The buffer values are different for FDI and DP modes,
 * but the HDMI/DVI fields are shared among those. So we program the DDI
 * in either FDI or DP modes only, as HDMI connections will work with both
 * of those
 */
void intel_prepare_ddi_buffers(struct drm_device *dev, enum port port, bool use_fdi_mode)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 reg;
	int i;
	const u32 *ddi_translations = ((use_fdi_mode) ?
		hsw_ddi_translations_fdi :
		hsw_ddi_translations_dp);

	DRM_DEBUG_DRIVER("Initializing DDI buffers for port %c in %s mode\n",
			port_name(port),
			use_fdi_mode ? "FDI" : "DP");

	WARN((use_fdi_mode && (port != PORT_E)),
		"Programming port %c in FDI mode, this probably will not work.\n",
		port_name(port));

	for (i=0, reg=DDI_BUF_TRANS(port); i < ARRAY_SIZE(hsw_ddi_translations_fdi); i++) {
		I915_WRITE(reg, ddi_translations[i]);
		reg += 4;
	}
}

/* Program DDI buffers translations for DP. By default, program ports A-D in DP
 * mode and port E for FDI.
 */
void intel_prepare_ddi(struct drm_device *dev)
{
	int port;

	if (IS_HASWELL(dev)) {
		for (port = PORT_A; port < PORT_E; port++)
			intel_prepare_ddi_buffers(dev, port, false);

		/* DDI E is the suggested one to work in FDI mode, so program is as such by
		 * default. It will have to be re-programmed in case a digital DP output
		 * will be detected on it
		 */
		intel_prepare_ddi_buffers(dev, PORT_E, true);
	}
}

static const long hsw_ddi_buf_ctl_values[] = {
	DDI_BUF_EMP_400MV_0DB_HSW,
	DDI_BUF_EMP_400MV_3_5DB_HSW,
	DDI_BUF_EMP_400MV_6DB_HSW,
	DDI_BUF_EMP_400MV_9_5DB_HSW,
	DDI_BUF_EMP_600MV_0DB_HSW,
	DDI_BUF_EMP_600MV_3_5DB_HSW,
	DDI_BUF_EMP_600MV_6DB_HSW,
	DDI_BUF_EMP_800MV_0DB_HSW,
	DDI_BUF_EMP_800MV_3_5DB_HSW
};


/* Starting with Haswell, different DDI ports can work in FDI mode for
 * connection to the PCH-located connectors. For this, it is necessary to train
 * both the DDI port and PCH receiver for the desired DDI buffer settings.
 *
 * The recommended port to work in FDI mode is DDI E, which we use here. Also,
 * please note that when FDI mode is active on DDI E, it shares 2 lines with
 * DDI A (which is used for eDP)
 */

void hsw_fdi_link_train(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;
	u32 reg, temp, i;

	/* Start the training iterating through available voltages and emphasis */
	for (i=0; i < ARRAY_SIZE(hsw_ddi_buf_ctl_values); i++) {
		/* Configure DP_TP_CTL with auto-training */
		I915_WRITE(DP_TP_CTL(PORT_E),
					DP_TP_CTL_FDI_AUTOTRAIN |
					DP_TP_CTL_ENHANCED_FRAME_ENABLE |
					DP_TP_CTL_LINK_TRAIN_PAT1 |
					DP_TP_CTL_ENABLE);

		/* Configure and enable DDI_BUF_CTL for DDI E with next voltage */
		temp = I915_READ(DDI_BUF_CTL(PORT_E));
		temp = (temp & ~DDI_BUF_EMP_MASK);
		I915_WRITE(DDI_BUF_CTL(PORT_E),
				temp |
				DDI_BUF_CTL_ENABLE |
				DDI_PORT_WIDTH_X2 |
				hsw_ddi_buf_ctl_values[i]);

		udelay(600);

		/* We need to program FDI_RX_MISC with the default TP1 to TP2
		 * values before enabling the receiver, and configure the delay
		 * for the FDI timing generator to 90h. Luckily, all the other
		 * bits are supposed to be zeroed, so we can write those values
		 * directly.
		 */
		I915_WRITE(FDI_RX_MISC(pipe), FDI_RX_TP1_TO_TP2_48 |
				FDI_RX_FDI_DELAY_90);

		/* Enable CPU FDI Receiver with auto-training */
		reg = FDI_RX_CTL(pipe);
		I915_WRITE(reg,
				I915_READ(reg) |
					FDI_LINK_TRAIN_AUTO |
					FDI_RX_ENABLE |
					FDI_LINK_TRAIN_PATTERN_1_CPT |
					FDI_RX_ENHANCE_FRAME_ENABLE |
					FDI_PORT_WIDTH_2X_LPT |
					FDI_RX_PLL_ENABLE);
		POSTING_READ(reg);
		udelay(100);

		temp = I915_READ(DP_TP_STATUS(PORT_E));
		if (temp & DP_TP_STATUS_AUTOTRAIN_DONE) {
			DRM_DEBUG_DRIVER("BUF_CTL training done on %d step\n", i);

			/* Enable normal pixel sending for FDI */
			I915_WRITE(DP_TP_CTL(PORT_E),
						DP_TP_CTL_FDI_AUTOTRAIN |
						DP_TP_CTL_LINK_TRAIN_NORMAL |
						DP_TP_CTL_ENHANCED_FRAME_ENABLE |
						DP_TP_CTL_ENABLE);

			break;
		} else {
			DRM_ERROR("Error training BUF_CTL %d\n", i);

			/* Disable DP_TP_CTL and FDI_RX_CTL) and retry */
			I915_WRITE(DP_TP_CTL(PORT_E),
					I915_READ(DP_TP_CTL(PORT_E)) &
						~DP_TP_CTL_ENABLE);
			I915_WRITE(FDI_RX_CTL(pipe),
					I915_READ(FDI_RX_CTL(pipe)) &
						~FDI_RX_PLL_ENABLE);
			continue;
		}
	}

	DRM_DEBUG_KMS("FDI train done.\n");
}

/* For DDI connections, it is possible to support different outputs over the
 * same DDI port, such as HDMI or DP or even VGA via FDI. So we don't know by
 * the time the output is detected what exactly is on the other end of it. This
 * function aims at providing support for this detection and proper output
 * configuration.
 */
void intel_ddi_init(struct drm_device *dev, enum port port)
{
	/* For now, we don't do any proper output detection and assume that we
	 * handle HDMI only */

	switch(port){
	case PORT_A:
		/* We don't handle eDP and DP yet */
		DRM_DEBUG_DRIVER("Found digital output on DDI port A\n");
		break;
	/* Assume that the  ports B, C and D are working in HDMI mode for now */
	case PORT_B:
	case PORT_C:
	case PORT_D:
		intel_hdmi_init(dev, DDI_BUF_CTL(port), port);
		break;
	default:
		DRM_DEBUG_DRIVER("No handlers defined for port %d, skipping DDI initialization\n",
				port);
		break;
	}
}

/* WRPLL clock dividers */
struct wrpll_tmds_clock {
	u32 clock;
	u16 p;		/* Post divider */
	u16 n2;		/* Feedback divider */
	u16 r2;		/* Reference divider */
};

/* Table of matching values for WRPLL clocks programming for each frequency.
 * The code assumes this table is sorted. */
static const struct wrpll_tmds_clock wrpll_tmds_clock_table[] = {
	{19750,	38,	25,	18},
	{20000,	48,	32,	18},
	{21000,	36,	21,	15},
	{21912,	42,	29,	17},
	{22000,	36,	22,	15},
	{23000,	36,	23,	15},
	{23500,	40,	40,	23},
	{23750,	26,	16,	14},
	{24000,	36,	24,	15},
	{25000,	36,	25,	15},
	{25175,	26,	40,	33},
	{25200,	30,	21,	15},
	{26000,	36,	26,	15},
	{27000,	30,	21,	14},
	{27027,	18,	100,	111},
	{27500,	30,	29,	19},
	{28000,	34,	30,	17},
	{28320,	26,	30,	22},
	{28322,	32,	42,	25},
	{28750,	24,	23,	18},
	{29000,	30,	29,	18},
	{29750,	32,	30,	17},
	{30000,	30,	25,	15},
	{30750,	30,	41,	24},
	{31000,	30,	31,	18},
	{31500,	30,	28,	16},
	{32000,	30,	32,	18},
	{32500,	28,	32,	19},
	{33000,	24,	22,	15},
	{34000,	28,	30,	17},
	{35000,	26,	32,	19},
	{35500,	24,	30,	19},
	{36000,	26,	26,	15},
	{36750,	26,	46,	26},
	{37000,	24,	23,	14},
	{37762,	22,	40,	26},
	{37800,	20,	21,	15},
	{38000,	24,	27,	16},
	{38250,	24,	34,	20},
	{39000,	24,	26,	15},
	{40000,	24,	32,	18},
	{40500,	20,	21,	14},
	{40541,	22,	147,	89},
	{40750,	18,	19,	14},
	{41000,	16,	17,	14},
	{41500,	22,	44,	26},
	{41540,	22,	44,	26},
	{42000,	18,	21,	15},
	{42500,	22,	45,	26},
	{43000,	20,	43,	27},
	{43163,	20,	24,	15},
	{44000,	18,	22,	15},
	{44900,	20,	108,	65},
	{45000,	20,	25,	15},
	{45250,	20,	52,	31},
	{46000,	18,	23,	15},
	{46750,	20,	45,	26},
	{47000,	20,	40,	23},
	{48000,	18,	24,	15},
	{49000,	18,	49,	30},
	{49500,	16,	22,	15},
	{50000,	18,	25,	15},
	{50500,	18,	32,	19},
	{51000,	18,	34,	20},
	{52000,	18,	26,	15},
	{52406,	14,	34,	25},
	{53000,	16,	22,	14},
	{54000,	16,	24,	15},
	{54054,	16,	173,	108},
	{54500,	14,	24,	17},
	{55000,	12,	22,	18},
	{56000,	14,	45,	31},
	{56250,	16,	25,	15},
	{56750,	14,	25,	17},
	{57000,	16,	27,	16},
	{58000,	16,	43,	25},
	{58250,	16,	38,	22},
	{58750,	16,	40,	23},
	{59000,	14,	26,	17},
	{59341,	14,	40,	26},
	{59400,	16,	44,	25},
	{60000,	16,	32,	18},
	{60500,	12,	39,	29},
	{61000,	14,	49,	31},
	{62000,	14,	37,	23},
	{62250,	14,	42,	26},
	{63000,	12,	21,	15},
	{63500,	14,	28,	17},
	{64000,	12,	27,	19},
	{65000,	14,	32,	19},
	{65250,	12,	29,	20},
	{65500,	12,	32,	22},
	{66000,	12,	22,	15},
	{66667,	14,	38,	22},
	{66750,	10,	21,	17},
	{67000,	14,	33,	19},
	{67750,	14,	58,	33},
	{68000,	14,	30,	17},
	{68179,	14,	46,	26},
	{68250,	14,	46,	26},
	{69000,	12,	23,	15},
	{70000,	12,	28,	18},
	{71000,	12,	30,	19},
	{72000,	12,	24,	15},
	{73000,	10,	23,	17},
	{74000,	12,	23,	14},
	{74176,	8,	100,	91},
	{74250,	10,	22,	16},
	{74481,	12,	43,	26},
	{74500,	10,	29,	21},
	{75000,	12,	25,	15},
	{75250,	10,	39,	28},
	{76000,	12,	27,	16},
	{77000,	12,	53,	31},
	{78000,	12,	26,	15},
	{78750,	12,	28,	16},
	{79000,	10,	38,	26},
	{79500,	10,	28,	19},
	{80000,	12,	32,	18},
	{81000,	10,	21,	14},
	{81081,	6,	100,	111},
	{81624,	8,	29,	24},
	{82000,	8,	17,	14},
	{83000,	10,	40,	26},
	{83950,	10,	28,	18},
	{84000,	10,	28,	18},
	{84750,	6,	16,	17},
	{85000,	6,	17,	18},
	{85250,	10,	30,	19},
	{85750,	10,	27,	17},
	{86000,	10,	43,	27},
	{87000,	10,	29,	18},
	{88000,	10,	44,	27},
	{88500,	10,	41,	25},
	{89000,	10,	28,	17},
	{89012,	6,	90,	91},
	{89100,	10,	33,	20},
	{90000,	10,	25,	15},
	{91000,	10,	32,	19},
	{92000,	10,	46,	27},
	{93000,	10,	31,	18},
	{94000,	10,	40,	23},
	{94500,	10,	28,	16},
	{95000,	10,	44,	25},
	{95654,	10,	39,	22},
	{95750,	10,	39,	22},
	{96000,	10,	32,	18},
	{97000,	8,	23,	16},
	{97750,	8,	42,	29},
	{98000,	8,	45,	31},
	{99000,	8,	22,	15},
	{99750,	8,	34,	23},
	{100000,	6,	20,	18},
	{100500,	6,	19,	17},
	{101000,	6,	37,	33},
	{101250,	8,	21,	14},
	{102000,	6,	17,	15},
	{102250,	6,	25,	22},
	{103000,	8,	29,	19},
	{104000,	8,	37,	24},
	{105000,	8,	28,	18},
	{106000,	8,	22,	14},
	{107000,	8,	46,	29},
	{107214,	8,	27,	17},
	{108000,	8,	24,	15},
	{108108,	8,	173,	108},
	{109000,	6,	23,	19},
	{110000,	6,	22,	18},
	{110013,	6,	22,	18},
	{110250,	8,	49,	30},
	{110500,	8,	36,	22},
	{111000,	8,	23,	14},
	{111264,	8,	150,	91},
	{111375,	8,	33,	20},
	{112000,	8,	63,	38},
	{112500,	8,	25,	15},
	{113100,	8,	57,	34},
	{113309,	8,	42,	25},
	{114000,	8,	27,	16},
	{115000,	6,	23,	18},
	{116000,	8,	43,	25},
	{117000,	8,	26,	15},
	{117500,	8,	40,	23},
	{118000,	6,	38,	29},
	{119000,	8,	30,	17},
	{119500,	8,	46,	26},
	{119651,	8,	39,	22},
	{120000,	8,	32,	18},
	{121000,	6,	39,	29},
	{121250,	6,	31,	23},
	{121750,	6,	23,	17},
	{122000,	6,	42,	31},
	{122614,	6,	30,	22},
	{123000,	6,	41,	30},
	{123379,	6,	37,	27},
	{124000,	6,	51,	37},
	{125000,	6,	25,	18},
	{125250,	4,	13,	14},
	{125750,	4,	27,	29},
	{126000,	6,	21,	15},
	{127000,	6,	24,	17},
	{127250,	6,	41,	29},
	{128000,	6,	27,	19},
	{129000,	6,	43,	30},
	{129859,	4,	25,	26},
	{130000,	6,	26,	18},
	{130250,	6,	42,	29},
	{131000,	6,	32,	22},
	{131500,	6,	38,	26},
	{131850,	6,	41,	28},
	{132000,	6,	22,	15},
	{132750,	6,	28,	19},
	{133000,	6,	34,	23},
	{133330,	6,	37,	25},
	{134000,	6,	61,	41},
	{135000,	6,	21,	14},
	{135250,	6,	167,	111},
	{136000,	6,	62,	41},
	{137000,	6,	35,	23},
	{138000,	6,	23,	15},
	{138500,	6,	40,	26},
	{138750,	6,	37,	24},
	{139000,	6,	34,	22},
	{139050,	6,	34,	22},
	{139054,	6,	34,	22},
	{140000,	6,	28,	18},
	{141000,	6,	36,	23},
	{141500,	6,	22,	14},
	{142000,	6,	30,	19},
	{143000,	6,	27,	17},
	{143472,	4,	17,	16},
	{144000,	6,	24,	15},
	{145000,	6,	29,	18},
	{146000,	6,	47,	29},
	{146250,	6,	26,	16},
	{147000,	6,	49,	30},
	{147891,	6,	23,	14},
	{148000,	6,	23,	14},
	{148250,	6,	28,	17},
	{148352,	4,	100,	91},
	{148500,	6,	33,	20},
	{149000,	6,	48,	29},
	{150000,	6,	25,	15},
	{151000,	4,	19,	17},
	{152000,	6,	27,	16},
	{152280,	6,	44,	26},
	{153000,	6,	34,	20},
	{154000,	6,	53,	31},
	{155000,	6,	31,	18},
	{155250,	6,	50,	29},
	{155750,	6,	45,	26},
	{156000,	6,	26,	15},
	{157000,	6,	61,	35},
	{157500,	6,	28,	16},
	{158000,	6,	65,	37},
	{158250,	6,	44,	25},
	{159000,	6,	53,	30},
	{159500,	6,	39,	22},
	{160000,	6,	32,	18},
	{161000,	4,	31,	26},
	{162000,	4,	18,	15},
	{162162,	4,	131,	109},
	{162500,	4,	53,	44},
	{163000,	4,	29,	24},
	{164000,	4,	17,	14},
	{165000,	4,	22,	18},
	{166000,	4,	32,	26},
	{167000,	4,	26,	21},
	{168000,	4,	46,	37},
	{169000,	4,	104,	83},
	{169128,	4,	64,	51},
	{169500,	4,	39,	31},
	{170000,	4,	34,	27},
	{171000,	4,	19,	15},
	{172000,	4,	51,	40},
	{172750,	4,	32,	25},
	{172800,	4,	32,	25},
	{173000,	4,	41,	32},
	{174000,	4,	49,	38},
	{174787,	4,	22,	17},
	{175000,	4,	35,	27},
	{176000,	4,	30,	23},
	{177000,	4,	38,	29},
	{178000,	4,	29,	22},
	{178500,	4,	37,	28},
	{179000,	4,	53,	40},
	{179500,	4,	73,	55},
	{180000,	4,	20,	15},
	{181000,	4,	55,	41},
	{182000,	4,	31,	23},
	{183000,	4,	42,	31},
	{184000,	4,	30,	22},
	{184750,	4,	26,	19},
	{185000,	4,	37,	27},
	{186000,	4,	51,	37},
	{187000,	4,	36,	26},
	{188000,	4,	32,	23},
	{189000,	4,	21,	15},
	{190000,	4,	38,	27},
	{190960,	4,	41,	29},
	{191000,	4,	41,	29},
	{192000,	4,	27,	19},
	{192250,	4,	37,	26},
	{193000,	4,	20,	14},
	{193250,	4,	53,	37},
	{194000,	4,	23,	16},
	{194208,	4,	23,	16},
	{195000,	4,	26,	18},
	{196000,	4,	45,	31},
	{197000,	4,	35,	24},
	{197750,	4,	41,	28},
	{198000,	4,	22,	15},
	{198500,	4,	25,	17},
	{199000,	4,	28,	19},
	{200000,	4,	37,	25},
	{201000,	4,	61,	41},
	{202000,	4,	112,	75},
	{202500,	4,	21,	14},
	{203000,	4,	146,	97},
	{204000,	4,	62,	41},
	{204750,	4,	44,	29},
	{205000,	4,	38,	25},
	{206000,	4,	29,	19},
	{207000,	4,	23,	15},
	{207500,	4,	40,	26},
	{208000,	4,	37,	24},
	{208900,	4,	48,	31},
	{209000,	4,	48,	31},
	{209250,	4,	31,	20},
	{210000,	4,	28,	18},
	{211000,	4,	25,	16},
	{212000,	4,	22,	14},
	{213000,	4,	30,	19},
	{213750,	4,	38,	24},
	{214000,	4,	46,	29},
	{214750,	4,	35,	22},
	{215000,	4,	43,	27},
	{216000,	4,	24,	15},
	{217000,	4,	37,	23},
	{218000,	4,	42,	26},
	{218250,	4,	42,	26},
	{218750,	4,	34,	21},
	{219000,	4,	47,	29},
	{220000,	4,	44,	27},
	{220640,	4,	49,	30},
	{220750,	4,	36,	22},
	{221000,	4,	36,	22},
	{222000,	4,	23,	14},
	{222525,	4,	28,	17},
	{222750,	4,	33,	20},
	{227000,	4,	37,	22},
	{230250,	4,	29,	17},
	{233500,	4,	38,	22},
	{235000,	4,	40,	23},
	{238000,	4,	30,	17},
	{241500,	2,	17,	19},
	{245250,	2,	20,	22},
	{247750,	2,	22,	24},
	{253250,	2,	15,	16},
	{256250,	2,	18,	19},
	{262500,	2,	31,	32},
	{267250,	2,	66,	67},
	{268500,	2,	94,	95},
	{270000,	2,	14,	14},
	{272500,	2,	77,	76},
	{273750,	2,	57,	56},
	{280750,	2,	24,	23},
	{281250,	2,	23,	22},
	{286000,	2,	17,	16},
	{291750,	2,	26,	24},
	{296703,	2,	56,	51},
	{297000,	2,	22,	20},
	{298000,	2,	21,	19},
};

void intel_ddi_mode_set(struct drm_encoder *encoder,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	struct drm_crtc *crtc = encoder->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);
	int port = intel_hdmi->ddi_port;
	int pipe = intel_crtc->pipe;

	/* On Haswell, we need to enable the clocks and prepare DDI function to
	 * work in HDMI mode for this pipe.
	 */
	DRM_DEBUG_KMS("Preparing HDMI DDI mode for Haswell on port %c, pipe %c\n", port_name(port), pipe_name(pipe));

	if (intel_hdmi->has_audio) {
		/* Proper support for digital audio needs a new logic and a new set
		 * of registers, so we leave it for future patch bombing.
		 */
		DRM_DEBUG_DRIVER("HDMI audio on pipe %c on DDI\n",
				 pipe_name(intel_crtc->pipe));

		/* write eld */
		DRM_DEBUG_DRIVER("HDMI audio: write eld information\n");
		intel_write_eld(encoder, adjusted_mode);
	}

	intel_hdmi->set_infoframes(encoder, adjusted_mode);
}

static struct intel_encoder *
intel_ddi_get_crtc_encoder(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder, *ret = NULL;
	int num_encoders = 0;

	for_each_encoder_on_crtc(dev, crtc, intel_encoder) {
		ret = intel_encoder;
		num_encoders++;
	}

	if (num_encoders != 1)
		WARN(1, "%d encoders on crtc for pipe %d\n", num_encoders,
		     intel_crtc->pipe);

	BUG_ON(ret == NULL);
	return ret;
}

void intel_ddi_put_crtc_pll(struct drm_crtc *crtc)
{
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	struct intel_ddi_plls *plls = &dev_priv->ddi_plls;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	uint32_t val;

	switch (intel_crtc->ddi_pll_sel) {
	case PORT_CLK_SEL_SPLL:
		plls->spll_refcount--;
		if (plls->spll_refcount == 0) {
			DRM_DEBUG_KMS("Disabling SPLL\n");
			val = I915_READ(SPLL_CTL);
			WARN_ON(!(val & SPLL_PLL_ENABLE));
			I915_WRITE(SPLL_CTL, val & ~SPLL_PLL_ENABLE);
			POSTING_READ(SPLL_CTL);
		}
		break;
	case PORT_CLK_SEL_WRPLL1:
		plls->wrpll1_refcount--;
		if (plls->wrpll1_refcount == 0) {
			DRM_DEBUG_KMS("Disabling WRPLL 1\n");
			val = I915_READ(WRPLL_CTL1);
			WARN_ON(!(val & WRPLL_PLL_ENABLE));
			I915_WRITE(WRPLL_CTL1, val & ~WRPLL_PLL_ENABLE);
			POSTING_READ(WRPLL_CTL1);
		}
		break;
	case PORT_CLK_SEL_WRPLL2:
		plls->wrpll2_refcount--;
		if (plls->wrpll2_refcount == 0) {
			DRM_DEBUG_KMS("Disabling WRPLL 2\n");
			val = I915_READ(WRPLL_CTL2);
			WARN_ON(!(val & WRPLL_PLL_ENABLE));
			I915_WRITE(WRPLL_CTL2, val & ~WRPLL_PLL_ENABLE);
			POSTING_READ(WRPLL_CTL2);
		}
		break;
	}

	WARN(plls->spll_refcount < 0, "Invalid SPLL refcount\n");
	WARN(plls->wrpll1_refcount < 0, "Invalid WRPLL1 refcount\n");
	WARN(plls->wrpll2_refcount < 0, "Invalid WRPLL2 refcount\n");

	intel_crtc->ddi_pll_sel = PORT_CLK_SEL_NONE;
}

static void intel_ddi_calculate_wrpll(int clock, int *p, int *n2, int *r2)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(wrpll_tmds_clock_table); i++)
		if (clock <= wrpll_tmds_clock_table[i].clock)
			break;

	if (i == ARRAY_SIZE(wrpll_tmds_clock_table))
		i--;

	*p = wrpll_tmds_clock_table[i].p;
	*n2 = wrpll_tmds_clock_table[i].n2;
	*r2 = wrpll_tmds_clock_table[i].r2;

	if (wrpll_tmds_clock_table[i].clock != clock)
		DRM_INFO("WRPLL: using settings for %dKHz on %dKHz mode\n",
			 wrpll_tmds_clock_table[i].clock, clock);

	DRM_DEBUG_KMS("WRPLL: %dKHz refresh rate with p=%d, n2=%d r2=%d\n",
		      clock, *p, *n2, *r2);
}

bool intel_ddi_pll_mode_set(struct drm_crtc *crtc, int clock)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder = intel_ddi_get_crtc_encoder(crtc);
	struct drm_encoder *encoder = &intel_encoder->base;
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	struct intel_ddi_plls *plls = &dev_priv->ddi_plls;
	int type = intel_encoder->type;
	enum pipe pipe = intel_crtc->pipe;
	uint32_t reg, val;

	/* TODO: reuse PLLs when possible (compare values) */

	intel_ddi_put_crtc_pll(crtc);

	if (type == INTEL_OUTPUT_DISPLAYPORT || type == INTEL_OUTPUT_EDP) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		switch (intel_dp->link_bw) {
		case DP_LINK_BW_1_62:
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_LCPLL_810;
			break;
		case DP_LINK_BW_2_7:
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_LCPLL_1350;
			break;
		case DP_LINK_BW_5_4:
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_LCPLL_2700;
			break;
		default:
			DRM_ERROR("Link bandwidth %d unsupported\n",
				  intel_dp->link_bw);
			return false;
		}

		/* We don't need to turn any PLL on because we'll use LCPLL. */
		return true;

	} else if (type == INTEL_OUTPUT_HDMI) {
		int p, n2, r2;

		if (plls->wrpll1_refcount == 0) {
			DRM_DEBUG_KMS("Using WRPLL 1 on pipe %c\n",
				      pipe_name(pipe));
			plls->wrpll1_refcount++;
			reg = WRPLL_CTL1;
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_WRPLL1;
		} else if (plls->wrpll2_refcount == 0) {
			DRM_DEBUG_KMS("Using WRPLL 2 on pipe %c\n",
				      pipe_name(pipe));
			plls->wrpll2_refcount++;
			reg = WRPLL_CTL2;
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_WRPLL2;
		} else {
			DRM_ERROR("No WRPLLs available!\n");
			return false;
		}

		WARN(I915_READ(reg) & WRPLL_PLL_ENABLE,
		     "WRPLL already enabled\n");

		intel_ddi_calculate_wrpll(clock, &p, &n2, &r2);

		val = WRPLL_PLL_ENABLE | WRPLL_PLL_SELECT_LCPLL_2700 |
		      WRPLL_DIVIDER_REFERENCE(r2) | WRPLL_DIVIDER_FEEDBACK(n2) |
		      WRPLL_DIVIDER_POST(p);

	} else if (type == INTEL_OUTPUT_ANALOG) {
		if (plls->spll_refcount == 0) {
			DRM_DEBUG_KMS("Using SPLL on pipe %c\n",
				      pipe_name(pipe));
			plls->spll_refcount++;
			reg = SPLL_CTL;
			intel_crtc->ddi_pll_sel = PORT_CLK_SEL_SPLL;
		}

		WARN(I915_READ(reg) & SPLL_PLL_ENABLE,
		     "SPLL already enabled\n");

		val = SPLL_PLL_ENABLE | SPLL_PLL_FREQ_1350MHz | SPLL_PLL_SSC;

	} else {
		WARN(1, "Invalid DDI encoder type %d\n", type);
		return false;
	}

	I915_WRITE(reg, val);
	udelay(20);

	return true;
}

void intel_ddi_set_pipe_settings(struct drm_crtc *crtc)
{
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder = intel_ddi_get_crtc_encoder(crtc);
	enum pipe pipe = intel_crtc->pipe;
	int type = intel_encoder->type;
	uint32_t temp;

	if (type == INTEL_OUTPUT_DISPLAYPORT || type == INTEL_OUTPUT_EDP) {

		temp = PIPE_MSA_SYNC_CLK;
		switch (intel_crtc->bpp) {
		case 18:
			temp |= PIPE_MSA_6_BPC;
			break;
		case 24:
			temp |= PIPE_MSA_8_BPC;
			break;
		case 30:
			temp |= PIPE_MSA_10_BPC;
			break;
		case 36:
			temp |= PIPE_MSA_12_BPC;
			break;
		default:
			temp |= PIPE_MSA_8_BPC;
			WARN(1, "%d bpp unsupported by pipe DDI function\n",
			     intel_crtc->bpp);
		}
		I915_WRITE(PIPE_MSA_MISC(pipe), temp);
	}
}

void intel_ddi_enable_pipe_func(struct drm_crtc *crtc)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder = intel_ddi_get_crtc_encoder(crtc);
	struct drm_encoder *encoder = &intel_encoder->base;
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	enum pipe pipe = intel_crtc->pipe;
	int type = intel_encoder->type;
	uint32_t temp;

	/* Enable PIPE_DDI_FUNC_CTL for the pipe to work in HDMI mode */
	temp = PIPE_DDI_FUNC_ENABLE;

	switch (intel_crtc->bpp) {
	case 18:
		temp |= PIPE_DDI_BPC_6;
		break;
	case 24:
		temp |= PIPE_DDI_BPC_8;
		break;
	case 30:
		temp |= PIPE_DDI_BPC_10;
		break;
	case 36:
		temp |= PIPE_DDI_BPC_12;
		break;
	default:
		WARN(1, "%d bpp unsupported by pipe DDI function\n",
		     intel_crtc->bpp);
	}

	if (crtc->mode.flags & DRM_MODE_FLAG_PVSYNC)
		temp |= PIPE_DDI_PVSYNC;
	if (crtc->mode.flags & DRM_MODE_FLAG_PHSYNC)
		temp |= PIPE_DDI_PHSYNC;

	if (type == INTEL_OUTPUT_HDMI) {
		struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(encoder);

		if (intel_hdmi->has_hdmi_sink)
			temp |= PIPE_DDI_MODE_SELECT_HDMI;
		else
			temp |= PIPE_DDI_MODE_SELECT_DVI;

		temp |= PIPE_DDI_SELECT_PORT(intel_hdmi->ddi_port);

	} else if (type == INTEL_OUTPUT_ANALOG) {
		temp |= PIPE_DDI_MODE_SELECT_FDI;
		temp |= PIPE_DDI_SELECT_PORT(PORT_E);

	} else if (type == INTEL_OUTPUT_DISPLAYPORT ||
		   type == INTEL_OUTPUT_EDP) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		temp |= PIPE_DDI_MODE_SELECT_DP_SST;
		temp |= PIPE_DDI_SELECT_PORT(intel_dp->port);

		switch (intel_dp->lane_count) {
		case 1:
			temp |= PIPE_DDI_PORT_WIDTH_X1;
			break;
		case 2:
			temp |= PIPE_DDI_PORT_WIDTH_X2;
			break;
		case 4:
			temp |= PIPE_DDI_PORT_WIDTH_X4;
			break;
		default:
			temp |= PIPE_DDI_PORT_WIDTH_X4;
			WARN(1, "Unsupported lane count %d\n",
			     intel_dp->lane_count);
		}

	} else {
		WARN(1, "Invalid encoder type %d for pipe %d\n",
		     intel_encoder->type, pipe);
	}

	I915_WRITE(DDI_FUNC_CTL(pipe), temp);
}

void intel_ddi_disable_pipe_func(struct drm_i915_private *dev_priv,
				 enum pipe pipe)
{
	uint32_t reg = DDI_FUNC_CTL(pipe);
	uint32_t val = I915_READ(reg);

	val &= ~(PIPE_DDI_FUNC_ENABLE | PIPE_DDI_PORT_MASK);
	val |= PIPE_DDI_PORT_NONE;
	I915_WRITE(reg, val);
}

bool intel_ddi_get_hw_state(struct intel_encoder *encoder,
			    enum pipe *pipe)
{
	struct drm_device *dev = encoder->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	enum port port = intel_ddi_get_encoder_port(encoder);
	u32 tmp;
	int i;

	tmp = I915_READ(DDI_BUF_CTL(port));

	if (!(tmp & DDI_BUF_CTL_ENABLE))
		return false;

	for_each_pipe(i) {
		tmp = I915_READ(DDI_FUNC_CTL(i));

		if ((tmp & PIPE_DDI_PORT_MASK)
		    == PIPE_DDI_SELECT_PORT(port)) {
			*pipe = i;
			return true;
		}
	}

	DRM_DEBUG_KMS("No pipe for ddi port %i found\n", port);

	return true;
}

static uint32_t intel_ddi_get_crtc_pll(struct drm_i915_private *dev_priv,
				       enum pipe pipe)
{
	uint32_t temp, ret;
	enum port port;
	int i;

	temp = I915_READ(DDI_FUNC_CTL(pipe));
	temp &= PIPE_DDI_PORT_MASK;
	for (i = PORT_A; i <= PORT_E; i++)
		if (temp == PIPE_DDI_SELECT_PORT(i))
			port = i;

	ret = I915_READ(PORT_CLK_SEL(port));

	DRM_DEBUG_KMS("Pipe %c connected to port %c using clock 0x%08x\n",
		      pipe_name(pipe), port_name(port), ret);

	return ret;
}

void intel_ddi_setup_hw_pll_state(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	enum pipe pipe;
	struct intel_crtc *intel_crtc;

	for_each_pipe(pipe) {
		intel_crtc =
			to_intel_crtc(dev_priv->pipe_to_crtc_mapping[pipe]);

		if (!intel_crtc->active)
			continue;

		intel_crtc->ddi_pll_sel = intel_ddi_get_crtc_pll(dev_priv,
								 pipe);

		switch (intel_crtc->ddi_pll_sel) {
		case PORT_CLK_SEL_SPLL:
			dev_priv->ddi_plls.spll_refcount++;
			break;
		case PORT_CLK_SEL_WRPLL1:
			dev_priv->ddi_plls.wrpll1_refcount++;
			break;
		case PORT_CLK_SEL_WRPLL2:
			dev_priv->ddi_plls.wrpll2_refcount++;
			break;
		}
	}
}

void intel_ddi_enable_pipe_clock(struct intel_crtc *intel_crtc)
{
	struct drm_crtc *crtc = &intel_crtc->base;
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	struct intel_encoder *intel_encoder = intel_ddi_get_crtc_encoder(crtc);
	enum port port = intel_ddi_get_encoder_port(intel_encoder);

	I915_WRITE(PIPE_CLK_SEL(intel_crtc->pipe), PIPE_CLK_SEL_PORT(port));
}

void intel_ddi_disable_pipe_clock(struct intel_crtc *intel_crtc)
{
	struct drm_i915_private *dev_priv = intel_crtc->base.dev->dev_private;

	I915_WRITE(PIPE_CLK_SEL(intel_crtc->pipe), PIPE_CLK_SEL_DISABLED);
}

void intel_ddi_pre_enable(struct intel_encoder *intel_encoder)
{
	struct drm_crtc *crtc = intel_encoder->base.crtc;
	struct drm_i915_private *dev_priv = crtc->dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	enum port port = intel_ddi_get_encoder_port(intel_encoder);

	WARN_ON(intel_crtc->ddi_pll_sel == PORT_CLK_SEL_NONE);

	I915_WRITE(PORT_CLK_SEL(port), intel_crtc->ddi_pll_sel);
}

static void intel_wait_ddi_buf_idle(struct drm_i915_private *dev_priv,
				    enum port port)
{
	uint32_t reg = DDI_BUF_CTL(port);
	int i;

	for (i = 0; i < 8; i++) {
		udelay(1);
		if (I915_READ(reg) & DDI_BUF_IS_IDLE)
			return;
	}
	DRM_ERROR("Timeout waiting for DDI BUF %c idle bit\n", port_name(port));
}

void intel_ddi_post_disable(struct intel_encoder *intel_encoder)
{
	struct drm_encoder *encoder = &intel_encoder->base;
	struct drm_i915_private *dev_priv = encoder->dev->dev_private;
	enum port port = intel_ddi_get_encoder_port(intel_encoder);
	uint32_t val;

	val = I915_READ(DDI_BUF_CTL(port));
	if (val & DDI_BUF_CTL_ENABLE) {
		val &= ~DDI_BUF_CTL_ENABLE;
		I915_WRITE(DDI_BUF_CTL(port), val);
		intel_wait_ddi_buf_idle(dev_priv, port);
	}

	I915_WRITE(PORT_CLK_SEL(port), PORT_CLK_SEL_NONE);
}

void intel_enable_ddi(struct intel_encoder *encoder)
{
	struct drm_device *dev = encoder->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_hdmi *intel_hdmi = enc_to_intel_hdmi(&encoder->base);
	int port = intel_hdmi->ddi_port;

	/* Enable DDI_BUF_CTL. In HDMI/DVI mode, the port width,
	 * and swing/emphasis values are ignored so nothing special needs
	 * to be done besides enabling the port.
	 */
	I915_WRITE(DDI_BUF_CTL(port), DDI_BUF_CTL_ENABLE);
}

void intel_disable_ddi(struct intel_encoder *encoder)
{
	/* This will be needed in the future, so leave it here for now */
}

static int intel_ddi_get_cdclk_freq(struct drm_i915_private *dev_priv)
{
	if (I915_READ(HSW_FUSE_STRAP) & HSW_CDCLK_LIMIT)
		return 450;
	else if ((I915_READ(LCPLL_CTL) & LCPLL_CLK_FREQ_MASK) ==
		 LCPLL_CLK_FREQ_450)
		return 450;
	else
		return 540;
}

void intel_ddi_pll_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	uint32_t val = I915_READ(LCPLL_CTL);

	/* The LCPLL register should be turned on by the BIOS. For now let's
	 * just check its state and print errors in case something is wrong.
	 * Don't even try to turn it on.
	 */

	DRM_DEBUG_KMS("CDCLK running at %dMHz\n",
		      intel_ddi_get_cdclk_freq(dev_priv));

	if (val & LCPLL_CD_SOURCE_FCLK)
		DRM_ERROR("CDCLK source is not LCPLL\n");

	if (val & LCPLL_PLL_DISABLE)
		DRM_ERROR("LCPLL is disabled\n");
}
