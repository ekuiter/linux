/*
 *    Support for LGDT3306A - 8VSB/QAM-B
 *
 *    Copyright (C) 2013 Fred Richter <frichter@hauppauge.com>
 *    - driver structure based on lgdt3305.[ch] by Michael Krufky
 *    - code based on LG3306_V0.35 API by LG Electronics Inc.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <asm/div64.h>
#include <linux/dvb/frontend.h>
#include "dvb_math.h"
#include "lgdt3306a.h"


static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "set debug level (info=1, reg=2 (or-able))");

#define DBG_INFO 1
#define DBG_REG  2
#define DBG_DUMP 4 /* FGR - comment out to remove dump code */

#define lg_printk(kern, fmt, arg...)					\
	printk(kern "%s(): " fmt, __func__, ##arg)

#define lg_info(fmt, arg...)	printk(KERN_INFO "lgdt3306a: " fmt, ##arg)
#define lg_warn(fmt, arg...)	lg_printk(KERN_WARNING,       fmt, ##arg)
#define lg_err(fmt, arg...)	lg_printk(KERN_ERR,           fmt, ##arg)
#define lg_dbg(fmt, arg...) if (debug & DBG_INFO)			\
				lg_printk(KERN_DEBUG,         fmt, ##arg)
#define lg_reg(fmt, arg...) if (debug & DBG_REG)			\
				lg_printk(KERN_DEBUG,         fmt, ##arg)

#define lg_chkerr(ret)							\
({									\
	int __ret;							\
	__ret = (ret < 0);						\
	if (__ret)							\
		lg_err("error %d on line %d\n",	ret, __LINE__);		\
	__ret;								\
})

struct lgdt3306a_state {
	struct i2c_adapter *i2c_adap;
	const struct lgdt3306a_config *cfg;

	struct dvb_frontend frontend;

	fe_modulation_t current_modulation;
	u32 current_frequency;
	u32 snr;
};

/* -----------------------------------------------
 LG3306A Register Usage
   (LG does not really name the registers, so this code does not either)
 0000 -> 00FF Common control and status
 1000 -> 10FF Synchronizer control and status
 1F00 -> 1FFF Smart Antenna control and status
 2100 -> 21FF VSB Equalizer control and status
 2800 -> 28FF QAM Equalizer control and status
 3000 -> 30FF FEC control and status
 ---------------------------------------------- */

enum lgdt3306a_lock_status {
	LG3306_UNLOCK       = 0x00,
	LG3306_LOCK         = 0x01,
	LG3306_UNKNOWN_LOCK = 0xFF
};

enum lgdt3306a_neverlock_status {
	LG3306_NL_INIT    = 0x00,
	LG3306_NL_PROCESS = 0x01,
	LG3306_NL_LOCK    = 0x02,
	LG3306_NL_FAIL    = 0x03,
	LG3306_NL_UNKNOWN = 0xFF
};

enum lgdt3306a_modulation {
	LG3306_VSB          = 0x00,
	LG3306_QAM64        = 0x01,
	LG3306_QAM256       = 0x02,
	LG3306_UNKNOWN_MODE = 0xFF
};

enum lgdt3306a_lock_check {
	LG3306_SYNC_LOCK,
	LG3306_FEC_LOCK,
	LG3306_TR_LOCK,
	LG3306_AGC_LOCK,
};


#ifdef DBG_DUMP
static void lgdt3306a_DumpAllRegs(struct lgdt3306a_state *state);
static void lgdt3306a_DumpRegs(struct lgdt3306a_state *state);
#endif


static int lgdt3306a_write_reg(struct lgdt3306a_state *state, u16 reg, u8 val)
{
	int ret;
	u8 buf[] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = {
		.addr = state->cfg->i2c_addr, .flags = 0,
		.buf = buf, .len = 3,
	};

	lg_reg("reg: 0x%04x, val: 0x%02x\n", reg, val);

	ret = i2c_transfer(state->i2c_adap, &msg, 1);

	if (ret != 1) {
		lg_err("error (addr %02x %02x <- %02x, err = %i)\n",
		       msg.buf[0], msg.buf[1], msg.buf[2], ret);
		if (ret < 0)
			return ret;
		else
			return -EREMOTEIO;
	}
	return 0;
}

static int lgdt3306a_read_reg(struct lgdt3306a_state *state, u16 reg, u8 *val)
{
	int ret;
	u8 reg_buf[] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg[] = {
		{ .addr = state->cfg->i2c_addr,
		  .flags = 0, .buf = reg_buf, .len = 2 },
		{ .addr = state->cfg->i2c_addr,
		  .flags = I2C_M_RD, .buf = val, .len = 1 },
	};

	ret = i2c_transfer(state->i2c_adap, msg, 2);

	if (ret != 2) {
		lg_err("error (addr %02x reg %04x error (ret == %i)\n",
		       state->cfg->i2c_addr, reg, ret);
		if (ret < 0)
			return ret;
		else
			return -EREMOTEIO;
	}
	lg_reg("reg: 0x%04x, val: 0x%02x\n", reg, *val);

	return 0;
}

#define read_reg(state, reg)						\
({									\
	u8 __val;							\
	int ret = lgdt3306a_read_reg(state, reg, &__val);		\
	if (lg_chkerr(ret))						\
		__val = 0;						\
	__val;								\
})

static int lgdt3306a_set_reg_bit(struct lgdt3306a_state *state,
				u16 reg, int bit, int onoff)
{
	u8 val;
	int ret;

	lg_reg("reg: 0x%04x, bit: %d, level: %d\n", reg, bit, onoff);

	ret = lgdt3306a_read_reg(state, reg, &val);
	if (lg_chkerr(ret))
		goto fail;

	val &= ~(1 << bit);
	val |= (onoff & 1) << bit;

	ret = lgdt3306a_write_reg(state, reg, val);
	lg_chkerr(ret);
fail:
	return ret;
}

/* ------------------------------------------------------------------------ */

static int lgdt3306a_soft_reset(struct lgdt3306a_state *state)
{
	int ret;

	lg_dbg("\n");

	ret = lgdt3306a_set_reg_bit(state, 0x0000, 7, 0);
	if (lg_chkerr(ret))
		goto fail;

	msleep(20);
	ret = lgdt3306a_set_reg_bit(state, 0x0000, 7, 1);
	lg_chkerr(ret);

fail:
	return ret;
}

static int lgdt3306a_mpeg_mode(struct lgdt3306a_state *state,
				     enum lgdt3306a_mpeg_mode mode)
{
	u8 val;
	int ret;

	lg_dbg("(%d)\n", mode);
	/* transport packet format */
	ret = lgdt3306a_set_reg_bit(state, 0x0071, 7, mode == LGDT3306A_MPEG_PARALLEL?1:0); /* TPSENB=0x80 */
	if (lg_chkerr(ret))
		goto fail;

	/* start of packet signal duration */
	ret = lgdt3306a_set_reg_bit(state, 0x0071, 6, 0); /* TPSSOPBITEN=0x40; 0=byte duration, 1=bit duration */
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_read_reg(state, 0x0070, &val);
	if (lg_chkerr(ret))
		goto fail;

	val |= 0x10; /* TPCLKSUPB=0x10 */

	if (mode == LGDT3306A_MPEG_PARALLEL)
		val &= ~0x10;

	ret = lgdt3306a_write_reg(state, 0x0070, val);
	lg_chkerr(ret);

fail:
	return ret;
}

static int lgdt3306a_mpeg_mode_polarity(struct lgdt3306a_state *state,
				       enum lgdt3306a_tp_clock_edge edge,
				       enum lgdt3306a_tp_valid_polarity valid)
{
	u8 val;
	int ret;

	lg_dbg("edge=%d, valid=%d\n", edge, valid);

	ret = lgdt3306a_read_reg(state, 0x0070, &val);
	if (lg_chkerr(ret))
		goto fail;

	val &= ~0x06; /* TPCLKPOL=0x04, TPVALPOL=0x02 */

	if (edge == LGDT3306A_TPCLK_RISING_EDGE)
		val |= 0x04;
	if (valid == LGDT3306A_TP_VALID_HIGH)
		val |= 0x02;

	ret = lgdt3306a_write_reg(state, 0x0070, val);
	lg_chkerr(ret);

fail:
	return ret;
}

static int lgdt3306a_mpeg_tristate(struct lgdt3306a_state *state,
				     int mode)
{
	u8 val;
	int ret;

	lg_dbg("(%d)\n", mode);

	if (mode) {
		ret = lgdt3306a_read_reg(state, 0x0070, &val);
		if (lg_chkerr(ret))
			goto fail;
		val &= ~0xA8; /* Tristate bus; TPOUTEN=0x80, TPCLKOUTEN=0x20, TPDATAOUTEN=0x08 */
		ret = lgdt3306a_write_reg(state, 0x0070, val);
		if (lg_chkerr(ret))
			goto fail;

		ret = lgdt3306a_set_reg_bit(state, 0x0003, 6, 1); /* AGCIFOUTENB=0x40; 1=Disable IFAGC pin */
		if (lg_chkerr(ret))
			goto fail;

	} else {
		ret = lgdt3306a_set_reg_bit(state, 0x0003, 6, 0); /* enable IFAGC pin */
		if (lg_chkerr(ret))
			goto fail;

		ret = lgdt3306a_read_reg(state, 0x0070, &val);
		if (lg_chkerr(ret))
			goto fail;

		val |= 0xA8; /* enable bus */
		ret = lgdt3306a_write_reg(state, 0x0070, val);
		if (lg_chkerr(ret))
			goto fail;
	}

fail:
	return ret;
}

static int lgdt3306a_ts_bus_ctrl(struct dvb_frontend *fe, int acquire)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	lg_dbg("acquire=%d\n", acquire);

	return lgdt3306a_mpeg_tristate(state, acquire ? 0 : 1);

}

static int lgdt3306a_power(struct lgdt3306a_state *state,
				     int mode)
{
	int ret;

	lg_dbg("(%d)\n", mode);

	if (mode == 0) {
		ret = lgdt3306a_set_reg_bit(state, 0x0000, 7, 0); /* into reset */
		if (lg_chkerr(ret))
			goto fail;

		ret = lgdt3306a_set_reg_bit(state, 0x0000, 0, 0); /* power down */
		if (lg_chkerr(ret))
			goto fail;

	} else {
		ret = lgdt3306a_set_reg_bit(state, 0x0000, 7, 1); /* out of reset */
		if (lg_chkerr(ret))
			goto fail;

		ret = lgdt3306a_set_reg_bit(state, 0x0000, 0, 1); /* power up */
		if (lg_chkerr(ret))
			goto fail;
	}

#ifdef DBG_DUMP
	lgdt3306a_DumpAllRegs(state);
#endif
fail:
	return ret;
}


static int lgdt3306a_set_vsb(struct lgdt3306a_state *state)
{
	u8 val;
	int ret;

	lg_dbg("\n");

	/* 0. Spectrum inversion detection manual; spectrum inverted */
	ret = lgdt3306a_read_reg(state, 0x0002, &val);
	val &= 0xF7; /* SPECINVAUTO Off */
	val |= 0x04; /* SPECINV On */
	ret = lgdt3306a_write_reg(state, 0x0002, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 1. Selection of standard mode(0x08=QAM, 0x80=VSB) */
	ret = lgdt3306a_write_reg(state, 0x0008, 0x80);
	if (lg_chkerr(ret))
		goto fail;

	/* 2. Bandwidth mode for VSB(6MHz) */
	ret = lgdt3306a_read_reg(state, 0x0009, &val);
	val &= 0xE3;
	val |= 0x0C; /* STDOPDETTMODE[2:0]=3 */
	ret = lgdt3306a_write_reg(state, 0x0009, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 3. QAM mode detection mode(None) */
	ret = lgdt3306a_read_reg(state, 0x0009, &val);
	val &= 0xFC; /* STDOPDETCMODE[1:0]=0 */
	ret = lgdt3306a_write_reg(state, 0x0009, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 4. ADC sampling frequency rate(2x sampling) */
	ret = lgdt3306a_read_reg(state, 0x000D, &val);
	val &= 0xBF; /* SAMPLING4XFEN=0 */
	ret = lgdt3306a_write_reg(state, 0x000D, val);
	if (lg_chkerr(ret))
		goto fail;

#if 0
	/* FGR - disable any AICC filtering, testing only */

	ret = lgdt3306a_write_reg(state, 0x0024, 0x00);
	if (lg_chkerr(ret))
		goto fail;

	/* AICCFIXFREQ0 NT N-1(Video rejection) */
	ret = lgdt3306a_write_reg(state, 0x002E, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002F, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0030, 0x00);

	/* AICCFIXFREQ1 NT N-1(Audio rejection) */
	ret = lgdt3306a_write_reg(state, 0x002B, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002C, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002D, 0x00);

	/* AICCFIXFREQ2 NT Co-Channel(Video rejection) */
	ret = lgdt3306a_write_reg(state, 0x0028, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0029, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002A, 0x00);

	/* AICCFIXFREQ3 NT Co-Channel(Audio rejection) */
	ret = lgdt3306a_write_reg(state, 0x0025, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0026, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0027, 0x00);

#else
	/* FGR - this works well for HVR-1955,1975 */

	/* 5. AICCOPMODE  NT N-1 Adj. */
	ret = lgdt3306a_write_reg(state, 0x0024, 0x5A);
	if (lg_chkerr(ret))
		goto fail;

	/* AICCFIXFREQ0 NT N-1(Video rejection) */
	ret = lgdt3306a_write_reg(state, 0x002E, 0x5A);
	ret = lgdt3306a_write_reg(state, 0x002F, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0030, 0x00);

	/* AICCFIXFREQ1 NT N-1(Audio rejection) */
	ret = lgdt3306a_write_reg(state, 0x002B, 0x36);
	ret = lgdt3306a_write_reg(state, 0x002C, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002D, 0x00);

	/* AICCFIXFREQ2 NT Co-Channel(Video rejection) */
	ret = lgdt3306a_write_reg(state, 0x0028, 0x2A);
	ret = lgdt3306a_write_reg(state, 0x0029, 0x00);
	ret = lgdt3306a_write_reg(state, 0x002A, 0x00);

	/* AICCFIXFREQ3 NT Co-Channel(Audio rejection) */
	ret = lgdt3306a_write_reg(state, 0x0025, 0x06);
	ret = lgdt3306a_write_reg(state, 0x0026, 0x00);
	ret = lgdt3306a_write_reg(state, 0x0027, 0x00);
#endif

	ret = lgdt3306a_read_reg(state, 0x001E, &val);
	val &= 0x0F;
	val |= 0xA0;
	ret = lgdt3306a_write_reg(state, 0x001E, val);

	ret = lgdt3306a_write_reg(state, 0x0022, 0x08);

	ret = lgdt3306a_write_reg(state, 0x0023, 0xFF);

	ret = lgdt3306a_read_reg(state, 0x211F, &val);
	val &= 0xEF;
	ret = lgdt3306a_write_reg(state, 0x211F, val);

	ret = lgdt3306a_write_reg(state, 0x2173, 0x01);

	ret = lgdt3306a_read_reg(state, 0x1061, &val);
	val &= 0xF8;
	val |= 0x04;
	ret = lgdt3306a_write_reg(state, 0x1061, val);

	ret = lgdt3306a_read_reg(state, 0x103D, &val);
	val &= 0xCF;
	ret = lgdt3306a_write_reg(state, 0x103D, val);

	ret = lgdt3306a_write_reg(state, 0x2122, 0x40);

	ret = lgdt3306a_read_reg(state, 0x2141, &val);
	val &= 0x3F;
	ret = lgdt3306a_write_reg(state, 0x2141, val);

	ret = lgdt3306a_read_reg(state, 0x2135, &val);
	val &= 0x0F;
	val |= 0x70;
	ret = lgdt3306a_write_reg(state, 0x2135, val);

	ret = lgdt3306a_read_reg(state, 0x0003, &val);
	val &= 0xF7;
	ret = lgdt3306a_write_reg(state, 0x0003, val);

	ret = lgdt3306a_read_reg(state, 0x001C, &val);
	val &= 0x7F;
	ret = lgdt3306a_write_reg(state, 0x001C, val);

	/* 6. EQ step size */
	ret = lgdt3306a_read_reg(state, 0x2179, &val);
	val &= 0xF8;
	ret = lgdt3306a_write_reg(state, 0x2179, val);

	ret = lgdt3306a_read_reg(state, 0x217A, &val);
	val &= 0xF8;
	ret = lgdt3306a_write_reg(state, 0x217A, val);

	/* 7. Reset */
	ret = lgdt3306a_soft_reset(state);
	if (lg_chkerr(ret))
		goto fail;

	lg_dbg("complete\n");
fail:
	return ret;
}

static int lgdt3306a_set_qam(struct lgdt3306a_state *state, int modulation)
{
	u8 val;
	int ret;

	lg_dbg("modulation=%d\n", modulation);

	/* 1. Selection of standard mode(0x08=QAM, 0x80=VSB) */
	ret = lgdt3306a_write_reg(state, 0x0008, 0x08);
	if (lg_chkerr(ret))
		goto fail;

	/* 1a. Spectrum inversion detection to Auto */
	ret = lgdt3306a_read_reg(state, 0x0002, &val);
	val &= 0xFB; /* SPECINV Off */
	val |= 0x08; /* SPECINVAUTO On */
	ret = lgdt3306a_write_reg(state, 0x0002, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 2. Bandwidth mode for QAM */
	ret = lgdt3306a_read_reg(state, 0x0009, &val);
	val &= 0xE3; /* STDOPDETTMODE[2:0]=0 VSB Off */
	ret = lgdt3306a_write_reg(state, 0x0009, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 3. : 64QAM/256QAM detection(manual, auto) */
	ret = lgdt3306a_read_reg(state, 0x0009, &val);
	val &= 0xFC;
	val |= 0x02; /* STDOPDETCMODE[1:0]=1=Manual 2=Auto */
	ret = lgdt3306a_write_reg(state, 0x0009, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 3a. : 64QAM/256QAM selection for manual */
	ret = lgdt3306a_read_reg(state, 0x101a, &val);
	val &= 0xF8;
	if (modulation == QAM_64)
		val |= 0x02; /* QMDQMODE[2:0]=2=QAM64 */
	else
		val |= 0x04; /* QMDQMODE[2:0]=4=QAM256 */

	ret = lgdt3306a_write_reg(state, 0x101a, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 4. ADC sampling frequency rate(4x sampling) */
	ret = lgdt3306a_read_reg(state, 0x000D, &val);
	val &= 0xBF;
	val |= 0x40; /* SAMPLING4XFEN=1 */
	ret = lgdt3306a_write_reg(state, 0x000D, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 5. No AICC operation in QAM mode */
	ret = lgdt3306a_read_reg(state, 0x0024, &val);
	val &= 0x00;
	ret = lgdt3306a_write_reg(state, 0x0024, val);
	if (lg_chkerr(ret))
		goto fail;

	/* 6. Reset */
	ret = lgdt3306a_soft_reset(state);
	if (lg_chkerr(ret))
		goto fail;

	lg_dbg("complete\n");
fail:
	return ret;
}

static int lgdt3306a_set_modulation(struct lgdt3306a_state *state,
				   struct dtv_frontend_properties *p)
{
	int ret;

	lg_dbg("\n");

	switch (p->modulation) {
	case VSB_8:
		ret = lgdt3306a_set_vsb(state);
		break;
	case QAM_64:
		ret = lgdt3306a_set_qam(state, QAM_64);
		break;
	case QAM_256:
		ret = lgdt3306a_set_qam(state, QAM_256);
		break;
	default:
		return -EINVAL;
	}
	if (lg_chkerr(ret))
		goto fail;

	state->current_modulation = p->modulation;

fail:
	return ret;
}

/* ------------------------------------------------------------------------ */

static int lgdt3306a_agc_setup(struct lgdt3306a_state *state,
			      struct dtv_frontend_properties *p)
{
	/* TODO: anything we want to do here??? */
	lg_dbg("\n");

	switch (p->modulation) {
	case VSB_8:
		break;
	case QAM_64:
	case QAM_256:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */

static int lgdt3306a_set_inversion(struct lgdt3306a_state *state,
				       int inversion)
{
	int ret;

	lg_dbg("(%d)\n", inversion);

	ret = lgdt3306a_set_reg_bit(state, 0x0002, 2, inversion ? 1 : 0);
	return ret;
}

static int lgdt3306a_set_inversion_auto(struct lgdt3306a_state *state,
				       int enabled)
{
	int ret;

	lg_dbg("(%d)\n", enabled);

	/* 0=Manual 1=Auto(QAM only) */
	ret = lgdt3306a_set_reg_bit(state, 0x0002, 3, enabled);/* SPECINVAUTO=0x04 */
	return ret;
}

static int lgdt3306a_spectral_inversion(struct lgdt3306a_state *state,
				       struct dtv_frontend_properties *p,
				       int inversion)
{
	int ret = 0;

	lg_dbg("(%d)\n", inversion);
#if 0
/* FGR - spectral_inversion defaults already set for VSB and QAM; can enable later if desired */

	ret = lgdt3306a_set_inversion(state, inversion);

	switch (p->modulation) {
	case VSB_8:
		ret = lgdt3306a_set_inversion_auto(state, 0); /* Manual only for VSB */
		break;
	case QAM_64:
	case QAM_256:
		ret = lgdt3306a_set_inversion_auto(state, 1); /* Auto ok for QAM */
		break;
	default:
		ret = -EINVAL;
	}
#endif
	return ret;
}

static int lgdt3306a_set_if(struct lgdt3306a_state *state,
			   struct dtv_frontend_properties *p)
{
	int ret;
	u16 if_freq_khz;
	u8 nco1, nco2;

	switch (p->modulation) {
	case VSB_8:
		if_freq_khz = state->cfg->vsb_if_khz;
		break;
	case QAM_64:
	case QAM_256:
		if_freq_khz = state->cfg->qam_if_khz;
		break;
	default:
		return -EINVAL;
	}

	switch (if_freq_khz) {
	default:
	    lg_warn("IF=%d KHz is not supportted, 3250 assumed\n", if_freq_khz);
		/* fallthrough */
	case 3250:  /* 3.25Mhz */
		nco1 = 0x34;
		nco2 = 0x00;
		break;
	case 3500:  /* 3.50Mhz */
		nco1 = 0x38;
		nco2 = 0x00;
		break;
	case 4000:  /* 4.00Mhz */
		nco1 = 0x40;
		nco2 = 0x00;
		break;
	case 5000:  /* 5.00Mhz */
		nco1 = 0x50;
		nco2 = 0x00;
		break;
	case 5380: /* 5.38Mhz */
		nco1 = 0x56;
		nco2 = 0x14;
		break;
	}
	ret = lgdt3306a_write_reg(state, 0x0010, nco1);
	ret = lgdt3306a_write_reg(state, 0x0011, nco2);

	lg_dbg("if_freq=%d KHz->[%04x]\n", if_freq_khz, nco1<<8 | nco2);

	return 0;
}

/* ------------------------------------------------------------------------ */

static int lgdt3306a_i2c_gate_ctrl(struct dvb_frontend *fe, int enable)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	if (state->cfg->deny_i2c_rptr) {
		lg_dbg("deny_i2c_rptr=%d\n", state->cfg->deny_i2c_rptr);
		return 0;
	}
	lg_dbg("(%d)\n", enable);

	return lgdt3306a_set_reg_bit(state, 0x0002, 7, enable ? 0 : 1); /* NI2CRPTEN=0x80 */
}

static int lgdt3306a_sleep(struct lgdt3306a_state *state)
{
	int ret;

	lg_dbg("\n");
	state->current_frequency = -1; /* force re-tune, when we wake */

	ret = lgdt3306a_mpeg_tristate(state, 1); /* disable data bus */
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_power(state, 0); /* power down */
	lg_chkerr(ret);

fail:
	return 0;
}

static int lgdt3306a_fe_sleep(struct dvb_frontend *fe)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	return lgdt3306a_sleep(state);
}

static int lgdt3306a_init(struct dvb_frontend *fe)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;
	u8 val;
	int ret;

	lg_dbg("\n");

	/* 1. Normal operation mode */
	ret = lgdt3306a_set_reg_bit(state, 0x0001, 0, 1); /* SIMFASTENB=0x01 */
	if (lg_chkerr(ret))
		goto fail;

	/* 2. Spectrum inversion auto detection (Not valid for VSB) */
	ret = lgdt3306a_set_inversion_auto(state, 0);
	if (lg_chkerr(ret))
		goto fail;

	/* 3. Spectrum inversion(According to the tuner configuration) */
	ret = lgdt3306a_set_inversion(state, 1);
	if (lg_chkerr(ret))
		goto fail;

	/* 4. Peak-to-peak voltage of ADC input signal */
	ret = lgdt3306a_set_reg_bit(state, 0x0004, 7, 1); /* ADCSEL1V=0x80=1Vpp; 0x00=2Vpp */
	if (lg_chkerr(ret))
		goto fail;

	/* 5. ADC output data capture clock phase */
	ret = lgdt3306a_set_reg_bit(state, 0x0004, 2, 0); /* 0=same phase as ADC clock */
	if (lg_chkerr(ret))
		goto fail;

	/* 5a. ADC sampling clock source */
	ret = lgdt3306a_set_reg_bit(state, 0x0004, 3, 0); /* ADCCLKPLLSEL=0x08; 0=use ext clock, not PLL */
	if (lg_chkerr(ret))
		goto fail;

	/* 6. Automatic PLL set */
	ret = lgdt3306a_set_reg_bit(state, 0x0005, 6, 0); /* PLLSETAUTO=0x40; 0=off */
	if (lg_chkerr(ret))
		goto fail;

	if (state->cfg->xtalMHz == 24) {	/* 24MHz */
		/* 7. Frequency for PLL output(0x2564 for 192MHz for 24MHz) */
		ret = lgdt3306a_read_reg(state, 0x0005, &val);
		if (lg_chkerr(ret))
			goto fail;
		val &= 0xC0;
		val |= 0x25;
		ret = lgdt3306a_write_reg(state, 0x0005, val);
		if (lg_chkerr(ret))
			goto fail;
		ret = lgdt3306a_write_reg(state, 0x0006, 0x64);
		if (lg_chkerr(ret))
			goto fail;

		/* 8. ADC sampling frequency(0x180000 for 24MHz sampling) */
		ret = lgdt3306a_read_reg(state, 0x000D, &val);
		if (lg_chkerr(ret))
			goto fail;
		val &= 0xC0;
		val |= 0x18;
		ret = lgdt3306a_write_reg(state, 0x000D, val);
		if (lg_chkerr(ret))
			goto fail;

	} else if (state->cfg->xtalMHz == 25) { /* 25MHz */
		/* 7. Frequency for PLL output */
		ret = lgdt3306a_read_reg(state, 0x0005, &val);
		if (lg_chkerr(ret))
			goto fail;
		val &= 0xC0;
		val |= 0x25;
		ret = lgdt3306a_write_reg(state, 0x0005, val);
		if (lg_chkerr(ret))
			goto fail;
		ret = lgdt3306a_write_reg(state, 0x0006, 0x64);
		if (lg_chkerr(ret))
			goto fail;

		/* 8. ADC sampling frequency(0x190000 for 25MHz sampling) */
		ret = lgdt3306a_read_reg(state, 0x000D, &val);
		if (lg_chkerr(ret))
			goto fail;
		val &= 0xC0;
		val |= 0x19;
		ret = lgdt3306a_write_reg(state, 0x000D, val);
		if (lg_chkerr(ret))
			goto fail;
	} else {
		lg_err("Bad xtalMHz=%d\n", state->cfg->xtalMHz);
	}
#if 0
	ret = lgdt3306a_write_reg(state, 0x000E, 0x00);
	ret = lgdt3306a_write_reg(state, 0x000F, 0x00);
#endif

	/* 9. Center frequency of input signal of ADC */
	ret = lgdt3306a_write_reg(state, 0x0010, 0x34); /* 3.25MHz */
	ret = lgdt3306a_write_reg(state, 0x0011, 0x00);

	/* 10. Fixed gain error value */
	ret = lgdt3306a_write_reg(state, 0x0014, 0); /* gain error=0 */

	/* 10a. VSB TR BW gear shift initial step */
	ret = lgdt3306a_read_reg(state, 0x103C, &val);
	val &= 0x0F;
	val |= 0x20; /* SAMGSAUTOSTL_V[3:0] = 2 */
	ret = lgdt3306a_write_reg(state, 0x103C, val);

	/* 10b. Timing offset calibration in low temperature for VSB */
	ret = lgdt3306a_read_reg(state, 0x103D, &val);
	val &= 0xFC;
	val |= 0x03;
	ret = lgdt3306a_write_reg(state, 0x103D, val);

	/* 10c. Timing offset calibration in low temperature for QAM */
	ret = lgdt3306a_read_reg(state, 0x1036, &val);
	val &= 0xF0;
	val |= 0x0C;
	ret = lgdt3306a_write_reg(state, 0x1036, val);

	/* 11. Using the imaginary part of CIR in CIR loading */
	ret = lgdt3306a_read_reg(state, 0x211F, &val);
	val &= 0xEF; /* do not use imaginary of CIR */
	ret = lgdt3306a_write_reg(state, 0x211F, val);

	/* 12. Control of no signal detector function */
	ret = lgdt3306a_read_reg(state, 0x2849, &val);
	val &= 0xEF; /* NOUSENOSIGDET=0, enable no signal detector */
	ret = lgdt3306a_write_reg(state, 0x2849, val);

	/* FGR - put demod in some known mode */
	ret = lgdt3306a_set_vsb(state);

	/* 13. TP stream format */
	ret = lgdt3306a_mpeg_mode(state, state->cfg->mpeg_mode);

	/* 14. disable output buses */
	ret = lgdt3306a_mpeg_tristate(state, 1);

	/* 15. Sleep (in reset) */
	ret = lgdt3306a_sleep(state);
	lg_chkerr(ret);

fail:
	return ret;
}

static int lgdt3306a_set_parameters(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;
	struct lgdt3306a_state *state = fe->demodulator_priv;
	int ret;

	lg_dbg("(%d, %d)\n", p->frequency, p->modulation);

	if (state->current_frequency  == p->frequency &&
	   state->current_modulation == p->modulation) {
		lg_dbg(" (already set, skipping ...)\n");
		return 0;
	}
	state->current_frequency = -1;
	state->current_modulation = -1;

	ret = lgdt3306a_power(state, 1); /* power up */
	if (lg_chkerr(ret))
		goto fail;

	if (fe->ops.tuner_ops.set_params) {
		ret = fe->ops.tuner_ops.set_params(fe);
		if (fe->ops.i2c_gate_ctrl)
			fe->ops.i2c_gate_ctrl(fe, 0);
#if 0
		if (lg_chkerr(ret))
			goto fail;
		state->current_frequency = p->frequency;
#endif
	}

	ret = lgdt3306a_set_modulation(state, p);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_agc_setup(state, p);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_set_if(state, p);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_spectral_inversion(state, p,
					  state->cfg->spectral_inversion ? 1 : 0);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_mpeg_mode(state, state->cfg->mpeg_mode);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_mpeg_mode_polarity(state,
					  state->cfg->tpclk_edge,
					  state->cfg->tpvalid_polarity);
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_mpeg_tristate(state, 0); /* enable data bus */
	if (lg_chkerr(ret))
		goto fail;

	ret = lgdt3306a_soft_reset(state);
	if (lg_chkerr(ret))
		goto fail;

#ifdef DBG_DUMP
	lgdt3306a_DumpAllRegs(state);
#endif
	state->current_frequency = p->frequency;
fail:
	return ret;
}

static int lgdt3306a_get_frontend(struct dvb_frontend *fe)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;

	lg_dbg("(%u, %d)\n", state->current_frequency, state->current_modulation);

	p->modulation = state->current_modulation;
	p->frequency = state->current_frequency;
	return 0;
}

static enum dvbfe_algo lgdt3306a_get_frontend_algo(struct dvb_frontend *fe)
{
#if 1
	return DVBFE_ALGO_CUSTOM;
#else
	return DVBFE_ALGO_HW;
#endif
}

/* ------------------------------------------------------------------------ */
static void lgdt3306a_monitor_vsb(struct lgdt3306a_state *state)
{
	u8 val;
	int ret;
	u8 snrRef, maxPowerMan, nCombDet;
	u16 fbDlyCir;

	ret = lgdt3306a_read_reg(state, 0x21A1, &val);
	snrRef = val & 0x3F;

	ret = lgdt3306a_read_reg(state, 0x2185, &maxPowerMan);

	ret = lgdt3306a_read_reg(state, 0x2191, &val);
	nCombDet = (val & 0x80) >> 7;

	ret = lgdt3306a_read_reg(state, 0x2180, &val);
	fbDlyCir = (val & 0x03) << 8;
	ret = lgdt3306a_read_reg(state, 0x2181, &val);
	fbDlyCir |= val;

	lg_dbg("snrRef=%d maxPowerMan=0x%x nCombDet=%d fbDlyCir=0x%x\n",
		snrRef, maxPowerMan, nCombDet, fbDlyCir);

	/* Carrier offset sub loop bandwidth */
	ret = lgdt3306a_read_reg(state, 0x1061, &val);
	val &= 0xF8;
	if ((snrRef > 18) && (maxPowerMan > 0x68) && (nCombDet == 0x01) && ((fbDlyCir == 0x03FF) || (fbDlyCir < 0x6C)))	{
		/* SNR is over 18dB and no ghosting */
		val |= 0x00; /* final bandwidth = 0 */
	} else {
		val |= 0x04; /* final bandwidth = 4 */
	}
	ret = lgdt3306a_write_reg(state, 0x1061, val);

	/* Adjust Notch Filter */
	ret = lgdt3306a_read_reg(state, 0x0024, &val);
	val &= 0x0F;
	if (nCombDet == 0) { /* Turn on the Notch Filter */
		val |= 0x50;
	}
	ret = lgdt3306a_write_reg(state, 0x0024, val);

	/* VSB Timing Recovery output normalization */
	ret = lgdt3306a_read_reg(state, 0x103D, &val);
	val &= 0xCF;
	val |= 0x20;
	ret = lgdt3306a_write_reg(state, 0x103D, val);
}

static enum lgdt3306a_modulation lgdt3306a_check_oper_mode(struct lgdt3306a_state *state)
{
	u8 val = 0;
	int ret;

	ret = lgdt3306a_read_reg(state, 0x0081, &val);

	if (val & 0x80)	{
		lg_dbg("VSB\n");
		return LG3306_VSB;
	}
	if (val & 0x08) {
		ret = lgdt3306a_read_reg(state, 0x00A6, &val);
		val = val >> 2;
		if (val & 0x01) {
			lg_dbg("QAM256\n");
			return LG3306_QAM256;
		} else {
			lg_dbg("QAM64\n");
			return LG3306_QAM64;
		}
	}
	lg_warn("UNKNOWN\n");
	return LG3306_UNKNOWN_MODE;
}

static enum lgdt3306a_lock_status lgdt3306a_check_lock_status(struct lgdt3306a_state *state,
			enum lgdt3306a_lock_check whatLock)
{
	u8 val = 0;
	int ret;
	enum lgdt3306a_modulation	modeOper;
	enum lgdt3306a_lock_status lockStatus;

	modeOper = LG3306_UNKNOWN_MODE;

	switch (whatLock) {
	case LG3306_SYNC_LOCK:
	{
		ret = lgdt3306a_read_reg(state, 0x00A6, &val);

		if ((val & 0x80) == 0x80)
			lockStatus = LG3306_LOCK;
		else
			lockStatus = LG3306_UNLOCK;

		lg_dbg("SYNC_LOCK=%x\n", lockStatus);
		break;
	}
	case LG3306_AGC_LOCK:
	{
		ret = lgdt3306a_read_reg(state, 0x0080, &val);

		if ((val & 0x40) == 0x40)
			lockStatus = LG3306_LOCK;
		else
			lockStatus = LG3306_UNLOCK;

		lg_dbg("AGC_LOCK=%x\n", lockStatus);
		break;
	}
	case LG3306_TR_LOCK:
	{
		modeOper = lgdt3306a_check_oper_mode(state);
		if ((modeOper == LG3306_QAM64) || (modeOper == LG3306_QAM256)) {
			ret = lgdt3306a_read_reg(state, 0x1094, &val);

			if ((val & 0x80) == 0x80)
				lockStatus = LG3306_LOCK;
			else
				lockStatus = LG3306_UNLOCK;
		} else
			lockStatus = LG3306_UNKNOWN_LOCK;

		lg_dbg("TR_LOCK=%x\n", lockStatus);
		break;
	}
	case LG3306_FEC_LOCK:
	{
		modeOper = lgdt3306a_check_oper_mode(state);
		if ((modeOper == LG3306_QAM64) || (modeOper == LG3306_QAM256)) {
			ret = lgdt3306a_read_reg(state, 0x0080, &val);

			if ((val & 0x10) == 0x10)
				lockStatus = LG3306_LOCK;
			else
				lockStatus = LG3306_UNLOCK;
		} else
			lockStatus = LG3306_UNKNOWN_LOCK;

		lg_dbg("FEC_LOCK=%x\n", lockStatus);
		break;
	}

	default:
		lockStatus = LG3306_UNKNOWN_LOCK;
		lg_warn("UNKNOWN whatLock=%d\n", whatLock);
		break;
	}

	return lockStatus;
}

static enum lgdt3306a_neverlock_status lgdt3306a_check_neverlock_status(struct lgdt3306a_state *state)
{
	u8 val = 0;
	int ret;
	enum lgdt3306a_neverlock_status lockStatus;

	ret = lgdt3306a_read_reg(state, 0x0080, &val);
	lockStatus = (enum lgdt3306a_neverlock_status)(val & 0x03);

	lg_dbg("NeverLock=%d", lockStatus);

	return lockStatus;
}

static void lgdt3306a_pre_monitoring(struct lgdt3306a_state *state)
{
	u8 val = 0;
	int ret;
	u8 currChDiffACQ, snrRef, mainStrong, aiccrejStatus;

	/* Channel variation */
	ret = lgdt3306a_read_reg(state, 0x21BC, &currChDiffACQ);

	/* SNR of Frame sync */
	ret = lgdt3306a_read_reg(state, 0x21A1, &val);
	snrRef = val & 0x3F;

	/* Strong Main CIR */
	ret = lgdt3306a_read_reg(state, 0x2199, &val);
	mainStrong = (val & 0x40) >> 6;

	ret = lgdt3306a_read_reg(state, 0x0090, &val);
	aiccrejStatus = (val & 0xF0) >> 4;

	lg_dbg("snrRef=%d mainStrong=%d aiccrejStatus=%d currChDiffACQ=0x%x\n",
		snrRef, mainStrong, aiccrejStatus, currChDiffACQ);

#if 0
	if ((mainStrong == 0) && (currChDiffACQ > 0x70)) /* Dynamic ghost exists */
#endif
	if (mainStrong == 0) {
		ret = lgdt3306a_read_reg(state, 0x2135, &val);
		val &= 0x0F;
		val |= 0xA0;
		ret = lgdt3306a_write_reg(state, 0x2135, val);

		ret = lgdt3306a_read_reg(state, 0x2141, &val);
		val &= 0x3F;
		val |= 0x80;
		ret = lgdt3306a_write_reg(state, 0x2141, val);

		ret = lgdt3306a_write_reg(state, 0x2122, 0x70);
	} else { /* Weak ghost or static channel */
		ret = lgdt3306a_read_reg(state, 0x2135, &val);
		val &= 0x0F;
		val |= 0x70;
		ret = lgdt3306a_write_reg(state, 0x2135, val);

		ret = lgdt3306a_read_reg(state, 0x2141, &val);
		val &= 0x3F;
		val |= 0x40;
		ret = lgdt3306a_write_reg(state, 0x2141, val);

		ret = lgdt3306a_write_reg(state, 0x2122, 0x40);
	}

}

static enum lgdt3306a_lock_status lgdt3306a_sync_lock_poll(struct lgdt3306a_state *state)
{
	enum lgdt3306a_lock_status syncLockStatus = LG3306_UNLOCK;
	int	i;

	for (i = 0; i < 2; i++)	{
		msleep(30);

		syncLockStatus = lgdt3306a_check_lock_status(state, LG3306_SYNC_LOCK);

		if (syncLockStatus == LG3306_LOCK) {
			lg_dbg("locked(%d)\n", i);
			return LG3306_LOCK;
		}
	}
	lg_dbg("not locked\n");
	return LG3306_UNLOCK;
}

static enum lgdt3306a_lock_status lgdt3306a_fec_lock_poll(struct lgdt3306a_state *state)
{
	enum lgdt3306a_lock_status FECLockStatus = LG3306_UNLOCK;
	int	i;

	for (i = 0; i < 2; i++)	{
		msleep(30);

		FECLockStatus = lgdt3306a_check_lock_status(state, LG3306_FEC_LOCK);

		if (FECLockStatus == LG3306_LOCK) {
			lg_dbg("locked(%d)\n", i);
			return FECLockStatus;
		}
	}
	lg_dbg("not locked\n");
	return FECLockStatus;
}

static enum lgdt3306a_neverlock_status lgdt3306a_neverlock_poll(struct lgdt3306a_state *state)
{
	enum lgdt3306a_neverlock_status NLLockStatus = LG3306_NL_FAIL;
	int	i;

	for (i = 0; i < 5; i++) {
		msleep(30);

		NLLockStatus = lgdt3306a_check_neverlock_status(state);

		if (NLLockStatus == LG3306_NL_LOCK) {
			lg_dbg("NL_LOCK(%d)\n", i);
			return NLLockStatus;
		}
	}
	lg_dbg("NLLockStatus=%d\n", NLLockStatus);
	return NLLockStatus;
}

static u8 lgdt3306a_get_packet_error(struct lgdt3306a_state *state)
{
	u8 val;
	int ret;

	ret = lgdt3306a_read_reg(state, 0x00FA, &val);

	return val;
}

static u32 log10_x1000(u32 x)
{
	static u32 valx_x10[]     = {  10,  11,  13,  15,  17,  20,  25,  33,  41,  50,  59,  73,  87,  100 };
	static u32 log10x_x1000[] = {   0,  41, 114, 176, 230, 301, 398, 518, 613, 699, 771, 863, 939, 1000 };
	static u32 nelems = sizeof(valx_x10)/sizeof(valx_x10[0]);
	u32 log_val = 0;
	u32 i;

	if (x <= 0)
		return -1000000; /* signal error */

	if (x < 10) {
		while (x < 10) {
			x = x * 10;
			log_val--;
		}
	} else if (x == 10) {
		return 0; /* log(1)=0 */
	} else {
		while (x >= 100) {
			x = x / 10;
			log_val++;
		}
	}
	log_val *= 1000;

	if (x == 10) /* was our input an exact multiple of 10 */
		return log_val;	/* don't need to interpolate */

	/* find our place on the log curve */
	for (i = 1; i < nelems; i++) {
		if (valx_x10[i] >= x)
			break;
	}

	{
		u32 diff_val   = x - valx_x10[i-1];
		u32 step_val   = valx_x10[i] - valx_x10[i-1];
		u32 step_log10 = log10x_x1000[i] - log10x_x1000[i-1];
		/* do a linear interpolation to get in-between values */
		return log_val + log10x_x1000[i-1] +
			((diff_val*step_log10) / step_val);
	}
}

static u32 lgdt3306a_calculate_snr_x100(struct lgdt3306a_state *state)
{
	u32 mse;  /* Mean-Square Error */
	u32 pwr;  /* Constelation power */
	u32 snr_x100;

	mse = (read_reg(state, 0x00EC) << 8) |
	      (read_reg(state, 0x00ED));
	pwr = (read_reg(state, 0x00E8) << 8) |
	      (read_reg(state, 0x00E9));

	if (mse == 0) /* no signal */
		return 0;

	snr_x100 = log10_x1000((pwr * 10000) / mse) - 3000;
	lg_dbg("mse=%u, pwr=%u, snr_x100=%d\n", mse, pwr, snr_x100);

	return snr_x100;
}

static enum lgdt3306a_lock_status lgdt3306a_vsb_lock_poll(struct lgdt3306a_state *state)
{
	u8 cnt = 0;
	u8 packet_error;
	u32 snr;

	while (1) {
		if (lgdt3306a_sync_lock_poll(state) == LG3306_UNLOCK) {
			lg_dbg("no sync lock!\n");
			return LG3306_UNLOCK;
		} else {
			msleep(20);
			lgdt3306a_pre_monitoring(state);

			packet_error = lgdt3306a_get_packet_error(state);
			snr = lgdt3306a_calculate_snr_x100(state);
			lg_dbg("cnt=%d errors=%d snr=%d\n",
			       cnt, packet_error, snr);

			if ((snr < 1500) || (packet_error >= 0xff))
				cnt++;
			else
				return LG3306_LOCK;

			if (cnt >= 10) {
				lg_dbg("not locked!\n");
				return LG3306_UNLOCK;
			}
		}
	}
	return LG3306_UNLOCK;
}

static enum lgdt3306a_lock_status lgdt3306a_qam_lock_poll(struct lgdt3306a_state *state)
{
	u8 cnt = 0;
	u8 packet_error;
	u32	snr;

	while (1) {
		if (lgdt3306a_fec_lock_poll(state) == LG3306_UNLOCK) {
			lg_dbg("no fec lock!\n");
			return LG3306_UNLOCK;
		} else {
			msleep(20);

			packet_error = lgdt3306a_get_packet_error(state);
			snr = lgdt3306a_calculate_snr_x100(state);
			lg_dbg("cnt=%d errors=%d snr=%d\n",
			       cnt, packet_error, snr);

			if ((snr < 1500) || (packet_error >= 0xff))
				cnt++;
			else
				return LG3306_LOCK;

			if (cnt >= 10) {
				lg_dbg("not locked!\n");
				return LG3306_UNLOCK;
			}
		}
	}
	return LG3306_UNLOCK;
}

static int lgdt3306a_read_status(struct dvb_frontend *fe, fe_status_t *status)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;
	u16 strength = 0;
	int ret = 0;

	if (fe->ops.tuner_ops.get_rf_strength) {
		ret = fe->ops.tuner_ops.get_rf_strength(fe, &strength);
		if (ret == 0) {
			lg_dbg("strength=%d\n", strength);
		} else {
			lg_dbg("fe->ops.tuner_ops.get_rf_strength() failed\n");
		}
	}

	*status = 0;
	if (lgdt3306a_neverlock_poll(state) == LG3306_NL_LOCK) {
		*status |= FE_HAS_SIGNAL;
		*status |= FE_HAS_CARRIER;

		switch (state->current_modulation) {
		case QAM_256:
		case QAM_64:
			if (lgdt3306a_qam_lock_poll(state) == LG3306_LOCK) {
				*status |= FE_HAS_VITERBI;
				*status |= FE_HAS_SYNC;

				*status |= FE_HAS_LOCK;
			}
			break;
		case VSB_8:
			if (lgdt3306a_vsb_lock_poll(state) == LG3306_LOCK) {
				*status |= FE_HAS_VITERBI;
				*status |= FE_HAS_SYNC;

				*status |= FE_HAS_LOCK;

				lgdt3306a_monitor_vsb(state);
			}
			break;
		default:
			ret = -EINVAL;
		}
	}
	return ret;
}


static int lgdt3306a_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	state->snr = lgdt3306a_calculate_snr_x100(state);
	/* report SNR in dB * 10 */
	*snr = state->snr/10;

	return 0;
}

static int lgdt3306a_read_signal_strength(struct dvb_frontend *fe,
					 u16 *strength)
{
	/*
	 * Calculate some sort of "strength" from SNR
	 */
	struct lgdt3306a_state *state = fe->demodulator_priv;
	u16 snr;  /* snr_x10 */
	int ret;
	u32 ref_snr; /* snr*100 */
	u32 str;

	*strength = 0;

	switch (state->current_modulation) {
	case VSB_8:
		 ref_snr = 1600; /* 16dB */
		 break;
	case QAM_64:
		 ref_snr = 2200; /* 22dB */
		 break;
	case QAM_256:
		 ref_snr = 2800; /* 28dB */
		 break;
	default:
		return -EINVAL;
	}

	ret = fe->ops.read_snr(fe, &snr);
	if (lg_chkerr(ret))
		goto fail;

	if (state->snr <= (ref_snr - 100))
		str = 0;
	else if (state->snr <= ref_snr)
		str = (0xffff * 65) / 100; /* 65% */
	else {
		str = state->snr - ref_snr;
		str /= 50;
		str += 78; /* 78%-100% */
		if (str > 100)
			str = 100;
		str = (0xffff * str) / 100;
	}
	*strength = (u16)str;
	lg_dbg("strength=%u\n", *strength);

fail:
	return ret;
}

/* ------------------------------------------------------------------------ */

static int lgdt3306a_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;
	u32 tmp;

	*ber = 0;
#if 1
	/* FGR - BUGBUG - I don't know what value is expected by dvb_core
	 * what is the scale of the value?? */
	tmp =              read_reg(state, 0x00FC); /* NBERVALUE[24-31] */
	tmp = (tmp << 8) | read_reg(state, 0x00FD); /* NBERVALUE[16-23] */
	tmp = (tmp << 8) | read_reg(state, 0x00FE); /* NBERVALUE[8-15] */
	tmp = (tmp << 8) | read_reg(state, 0x00FF); /* NBERVALUE[0-7] */
	*ber = tmp;
	lg_dbg("ber=%u\n", tmp);
#endif
	return 0;
}

static int lgdt3306a_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	*ucblocks = 0;
#if 1
	/* FGR - BUGBUG - I don't know what value is expected by dvb_core
	 * what happens when value wraps? */
	*ucblocks = read_reg(state, 0x00F4); /* TPIFTPERRCNT[0-7] */
	lg_dbg("ucblocks=%u\n", *ucblocks);
#endif

	return 0;
}

static int lgdt3306a_tune(struct dvb_frontend *fe, bool re_tune, unsigned int mode_flags, unsigned int *delay, fe_status_t *status)
{
	int ret = 0;
	struct lgdt3306a_state *state = fe->demodulator_priv;

	lg_dbg("re_tune=%u\n", re_tune);

	if (re_tune) {
		state->current_frequency = -1; /* force re-tune */
		ret = lgdt3306a_set_parameters(fe);
		if (ret != 0)
			return ret;
	}
	*delay = 125;
	ret = lgdt3306a_read_status(fe, status);

	return ret;
}

static int lgdt3306a_get_tune_settings(struct dvb_frontend *fe,
				       struct dvb_frontend_tune_settings
				       *fe_tune_settings)
{
	fe_tune_settings->min_delay_ms = 100;
	lg_dbg("\n");
	return 0;
}

static int lgdt3306a_search(struct dvb_frontend *fe)
{
	fe_status_t status = 0;
	int i, ret;

	/* set frontend */
	ret = lgdt3306a_set_parameters(fe);
	if (ret)
		goto error;

	/* wait frontend lock */
	for (i = 20; i > 0; i--) {
		lg_dbg(": loop=%d\n", i);
		msleep(50);
		ret = lgdt3306a_read_status(fe, &status);
		if (ret)
			goto error;

		if (status & FE_HAS_LOCK)
			break;
	}

	/* check if we have a valid signal */
	if (status & FE_HAS_LOCK)
		return DVBFE_ALGO_SEARCH_SUCCESS;
	else
		return DVBFE_ALGO_SEARCH_AGAIN;

error:
	lg_dbg("failed (%d)\n", ret);
	return DVBFE_ALGO_SEARCH_ERROR;
}

static void lgdt3306a_release(struct dvb_frontend *fe)
{
	struct lgdt3306a_state *state = fe->demodulator_priv;

	lg_dbg("\n");
	kfree(state);
}

static struct dvb_frontend_ops lgdt3306a_ops;

struct dvb_frontend *lgdt3306a_attach(const struct lgdt3306a_config *config,
				     struct i2c_adapter *i2c_adap)
{
	struct lgdt3306a_state *state = NULL;
	int ret;
	u8 val;

	lg_dbg("(%d-%04x)\n",
	       i2c_adap ? i2c_adapter_id(i2c_adap) : 0,
	       config ? config->i2c_addr : 0);

	state = kzalloc(sizeof(struct lgdt3306a_state), GFP_KERNEL);
	if (state == NULL)
		goto fail;

	state->cfg = config;
	state->i2c_adap = i2c_adap;

	memcpy(&state->frontend.ops, &lgdt3306a_ops,
	       sizeof(struct dvb_frontend_ops));
	state->frontend.demodulator_priv = state;

	/* verify that we're talking to a lg3306a */
	/* FGR - NOTE - there is no obvious ChipId to check; we check
	 * some "known" bits after reset, but it's still just a guess */
	ret = lgdt3306a_read_reg(state, 0x0000, &val);
	if (lg_chkerr(ret))
		goto fail;
	if ((val & 0x74) != 0x74) {
		lg_warn("expected 0x74, got 0x%x\n", (val & 0x74));
#if 0
		goto fail;	/* BUGBUG - re-enable when we know this is right */
#endif
	}
	ret = lgdt3306a_read_reg(state, 0x0001, &val);
	if (lg_chkerr(ret))
		goto fail;
	if ((val & 0xF6) != 0xC6) {
		lg_warn("expected 0xC6, got 0x%x\n", (val & 0xF6));
#if 0
		goto fail;	/* BUGBUG - re-enable when we know this is right */
#endif
	}
	ret = lgdt3306a_read_reg(state, 0x0002, &val);
	if (lg_chkerr(ret))
		goto fail;
	if ((val & 0x73) != 0x03) {
		lg_warn("expected 0x03, got 0x%x\n", (val & 0x73));
#if 0
		goto fail;	/* BUGBUG - re-enable when we know this is right */
#endif
	}

	state->current_frequency = -1;
	state->current_modulation = -1;

	lgdt3306a_sleep(state);

	return &state->frontend;

fail:
	lg_warn("unable to detect LGDT3306A hardware\n");
	kfree(state);
	return NULL;
}
EXPORT_SYMBOL(lgdt3306a_attach);

#ifdef DBG_DUMP

static const short regtab[] = {
	0x0000, /* SOFTRSTB 1'b1 1'b1 1'b1 ADCPDB 1'b1 PLLPDB GBBPDB 11111111 */
	0x0001, /* 1'b1 1'b1 1'b0 1'b0 AUTORPTRS */
	0x0002, /* NI2CRPTEN 1'b0 1'b0 1'b0 SPECINVAUT */
	0x0003, /* AGCRFOUT */
	0x0004, /* ADCSEL1V ADCCNT ADCCNF ADCCNS ADCCLKPLL */
	0x0005, /* PLLINDIVSE */
	0x0006, /* PLLCTRL[7:0] 11100001 */
	0x0007, /* SYSINITWAITTIME[7:0] (msec) 00001000 */
	0x0008, /* STDOPMODE[7:0] 10000000 */
	0x0009, /* 1'b0 1'b0 1'b0 STDOPDETTMODE[2:0] STDOPDETCMODE[1:0] 00011110 */
	0x000A, /* DAFTEN 1'b1 x x SCSYSLOCK */
	0x000B, /* SCSYSLOCKCHKTIME[7:0] (10msec) 01100100 */
	0x000D, /* x SAMPLING4 */
	0x000E, /* SAMFREQ[15:8] 00000000 */
	0x000F, /* SAMFREQ[7:0] 00000000 */
	0x0010, /* IFFREQ[15:8] 01100000 */
	0x0011, /* IFFREQ[7:0] 00000000 */
	0x0012, /* AGCEN AGCREFMO */
	0x0013, /* AGCRFFIXB AGCIFFIXB AGCLOCKDETRNGSEL[1:0] 1'b1 1'b0 1'b0 1'b0 11101000 */
	0x0014, /* AGCFIXVALUE[7:0] 01111111 */
	0x0015, /* AGCREF[15:8] 00001010 */
	0x0016, /* AGCREF[7:0] 11100100 */
	0x0017, /* AGCDELAY[7:0] 00100000 */
	0x0018, /* AGCRFBW[3:0] AGCIFBW[3:0] 10001000 */
	0x0019, /* AGCUDOUTMODE[1:0] AGCUDCTRLLEN[1:0] AGCUDCTRL */
	0x001C, /* 1'b1 PFEN MFEN AICCVSYNC */
	0x001D, /* 1'b0 1'b1 1'b0 1'b1 AICCVSYNC */
	0x001E, /* AICCALPHA[3:0] 1'b1 1'b0 1'b1 1'b0 01111010 */
	0x001F, /* AICCDETTH[19:16] AICCOFFTH[19:16] 00000000 */
	0x0020, /* AICCDETTH[15:8] 01111100 */
	0x0021, /* AICCDETTH[7:0] 00000000 */
	0x0022, /* AICCOFFTH[15:8] 00000101 */
	0x0023, /* AICCOFFTH[7:0] 11100000 */
	0x0024, /* AICCOPMODE3[1:0] AICCOPMODE2[1:0] AICCOPMODE1[1:0] AICCOPMODE0[1:0] 00000000 */
	0x0025, /* AICCFIXFREQ3[23:16] 00000000 */
	0x0026, /* AICCFIXFREQ3[15:8] 00000000 */
	0x0027, /* AICCFIXFREQ3[7:0] 00000000 */
	0x0028, /* AICCFIXFREQ2[23:16] 00000000 */
	0x0029, /* AICCFIXFREQ2[15:8] 00000000 */
	0x002A, /* AICCFIXFREQ2[7:0] 00000000 */
	0x002B, /* AICCFIXFREQ1[23:16] 00000000 */
	0x002C, /* AICCFIXFREQ1[15:8] 00000000 */
	0x002D, /* AICCFIXFREQ1[7:0] 00000000 */
	0x002E, /* AICCFIXFREQ0[23:16] 00000000 */
	0x002F, /* AICCFIXFREQ0[15:8] 00000000 */
	0x0030, /* AICCFIXFREQ0[7:0] 00000000 */
	0x0031, /* 1'b0 1'b1 1'b0 1'b0 x DAGC1STER */
	0x0032, /* DAGC1STEN DAGC1STER */
	0x0033, /* DAGC1STREF[15:8] 00001010 */
	0x0034, /* DAGC1STREF[7:0] 11100100 */
	0x0035, /* DAGC2NDE */
	0x0036, /* DAGC2NDREF[15:8] 00001010 */
	0x0037, /* DAGC2NDREF[7:0] 10000000 */
	0x0038, /* DAGC2NDLOCKDETRNGSEL[1:0] */
	0x003D, /* 1'b1 SAMGEARS */
	0x0040, /* SAMLFGMA */
	0x0041, /* SAMLFBWM */
	0x0044, /* 1'b1 CRGEARSHE */
	0x0045, /* CRLFGMAN */
	0x0046, /* CFLFBWMA */
	0x0047, /* CRLFGMAN */
	0x0048, /* x x x x CRLFGSTEP_VS[3:0] xxxx1001 */
	0x0049, /* CRLFBWMA */
	0x004A, /* CRLFBWMA */
	0x0050, /* 1'b0 1'b1 1'b1 1'b0 MSECALCDA */
	0x0070, /* TPOUTEN TPIFEN TPCLKOUTE */
	0x0071, /* TPSENB TPSSOPBITE */
	0x0073, /* TP47HINS x x CHBERINT PERMODE[1:0] PERINT[1:0] 1xx11100 */
	0x0075, /* x x x x x IQSWAPCTRL[2:0] xxxxx000 */
	0x0076, /* NBERCON NBERST NBERPOL NBERWSYN */
	0x0077, /* x NBERLOSTTH[2:0] NBERACQTH[3:0] x0000000 */
	0x0078, /* NBERPOLY[31:24] 00000000 */
	0x0079, /* NBERPOLY[23:16] 00000000 */
	0x007A, /* NBERPOLY[15:8] 00000000 */
	0x007B, /* NBERPOLY[7:0] 00000000 */
	0x007C, /* NBERPED[31:24] 00000000 */
	0x007D, /* NBERPED[23:16] 00000000 */
	0x007E, /* NBERPED[15:8] 00000000 */
	0x007F, /* NBERPED[7:0] 00000000 */
	0x0080, /* x AGCLOCK DAGCLOCK SYSLOCK x x NEVERLOCK[1:0] */
	0x0085, /* SPECINVST */
	0x0088, /* SYSLOCKTIME[15:8] */
	0x0089, /* SYSLOCKTIME[7:0] */
	0x008C, /* FECLOCKTIME[15:8] */
	0x008D, /* FECLOCKTIME[7:0] */
	0x008E, /* AGCACCOUT[15:8] */
	0x008F, /* AGCACCOUT[7:0] */
	0x0090, /* AICCREJSTATUS[3:0] AICCREJBUSY[3:0] */
	0x0091, /* AICCVSYNC */
	0x009C, /* CARRFREQOFFSET[15:8] */
	0x009D, /* CARRFREQOFFSET[7:0] */
	0x00A1, /* SAMFREQOFFSET[23:16] */
	0x00A2, /* SAMFREQOFFSET[15:8] */
	0x00A3, /* SAMFREQOFFSET[7:0] */
	0x00A6, /* SYNCLOCK SYNCLOCKH */
#if 0 /* covered elsewhere */
	0x00E8, /* CONSTPWR[15:8] */
	0x00E9, /* CONSTPWR[7:0] */
	0x00EA, /* BMSE[15:8] */
	0x00EB, /* BMSE[7:0] */
	0x00EC, /* MSE[15:8] */
	0x00ED, /* MSE[7:0] */
	0x00EE, /* CONSTI[7:0] */
	0x00EF, /* CONSTQ[7:0] */
#endif
	0x00F4, /* TPIFTPERRCNT[7:0] */
	0x00F5, /* TPCORREC */
	0x00F6, /* VBBER[15:8] */
	0x00F7, /* VBBER[7:0] */
	0x00F8, /* VABER[15:8] */
	0x00F9, /* VABER[7:0] */
	0x00FA, /* TPERRCNT[7:0] */
	0x00FB, /* NBERLOCK x x x x x x x */
	0x00FC, /* NBERVALUE[31:24] */
	0x00FD, /* NBERVALUE[23:16] */
	0x00FE, /* NBERVALUE[15:8] */
	0x00FF, /* NBERVALUE[7:0] */
	0x1000, /* 1'b0 WODAGCOU */
	0x1005, /* x x 1'b1 1'b1 x SRD_Q_QM */
	0x1009, /* SRDWAITTIME[7:0] (10msec) 00100011 */
	0x100A, /* SRDWAITTIME_CQS[7:0] (msec) 01100100 */
	0x101A, /* x 1'b1 1'b0 1'b0 x QMDQAMMODE[2:0] x100x010 */
	0x1036, /* 1'b0 1'b1 1'b0 1'b0 SAMGSEND_CQS[3:0] 01001110 */
	0x103C, /* SAMGSAUTOSTL_V[3:0] SAMGSAUTOEDL_V[3:0] 01000110 */
	0x103D, /* 1'b1 1'b1 SAMCNORMBP_V[1:0] 1'b0 1'b0 SAMMODESEL_V[1:0] 11100001 */
	0x103F, /* SAMZTEDSE */
	0x105D, /* EQSTATUSE */
	0x105F, /* x PMAPG2_V[2:0] x DMAPG2_V[2:0] x001x011 */
	0x1060, /* 1'b1 EQSTATUSE */
	0x1061, /* CRMAPBWSTL_V[3:0] CRMAPBWEDL_V[3:0] 00000100 */
	0x1065, /* 1'b0 x CRMODE_V[1:0] 1'b1 x 1'b1 x 0x111x1x */
	0x1066, /* 1'b0 1'b0 1'b1 1'b0 1'b1 PNBOOSTSE */
	0x1068, /* CREPHNGAIN2_V[3:0] CREPHNPBW_V[3:0] 10010001 */
	0x106E, /* x x x x x CREPHNEN_ */
	0x106F, /* CREPHNTH_V[7:0] 00010101 */
	0x1072, /* CRSWEEPN */
	0x1073, /* CRPGAIN_V[3:0] x x 1'b1 1'b1 1001xx11 */
	0x1074, /* CRPBW_V[3:0] x x 1'b1 1'b1 0001xx11 */
	0x1080, /* DAFTSTATUS[1:0] x x x x x x */
	0x1081, /* SRDSTATUS[1:0] x x x x x SRDLOCK */
	0x10A9, /* EQSTATUS_CQS[1:0] x x x x x x */
	0x10B7, /* EQSTATUS_V[1:0] x x x x x x */
#if 0 /* SMART_ANT */
	0x1F00, /* MODEDETE */
	0x1F01, /* x x x x x x x SFNRST xxxxxxx0 */
	0x1F03, /* NUMOFANT[7:0] 10000000 */
	0x1F04, /* x SELMASK[6:0] x0000000 */
	0x1F05, /* x SETMASK[6:0] x0000000 */
	0x1F06, /* x TXDATA[6:0] x0000000 */
	0x1F07, /* x CHNUMBER[6:0] x0000000 */
	0x1F09, /* AGCTIME[23:16] 10011000 */
	0x1F0A, /* AGCTIME[15:8] 10010110 */
	0x1F0B, /* AGCTIME[7:0] 10000000 */
	0x1F0C, /* ANTTIME[31:24] 00000000 */
	0x1F0D, /* ANTTIME[23:16] 00000011 */
	0x1F0E, /* ANTTIME[15:8] 10010000 */
	0x1F0F, /* ANTTIME[7:0] 10010000 */
	0x1F11, /* SYNCTIME[23:16] 10011000 */
	0x1F12, /* SYNCTIME[15:8] 10010110 */
	0x1F13, /* SYNCTIME[7:0] 10000000 */
	0x1F14, /* SNRTIME[31:24] 00000001 */
	0x1F15, /* SNRTIME[23:16] 01111101 */
	0x1F16, /* SNRTIME[15:8] 01111000 */
	0x1F17, /* SNRTIME[7:0] 01000000 */
	0x1F19, /* FECTIME[23:16] 00000000 */
	0x1F1A, /* FECTIME[15:8] 01110010 */
	0x1F1B, /* FECTIME[7:0] 01110000 */
	0x1F1D, /* FECTHD[7:0] 00000011 */
	0x1F1F, /* SNRTHD[23:16] 00001000 */
	0x1F20, /* SNRTHD[15:8] 01111111 */
	0x1F21, /* SNRTHD[7:0] 10000101 */
	0x1F80, /* IRQFLG x x SFSDRFLG MODEBFLG SAVEFLG SCANFLG TRACKFLG */
	0x1F81, /* x SYNCCON SNRCON FECCON x STDBUSY SYNCRST AGCFZCO */
	0x1F82, /* x x x SCANOPCD[4:0] */
	0x1F83, /* x x x x MAINOPCD[3:0] */
	0x1F84, /* x x RXDATA[13:8] */
	0x1F85, /* RXDATA[7:0] */
	0x1F86, /* x x SDTDATA[13:8] */
	0x1F87, /* SDTDATA[7:0] */
	0x1F89, /* ANTSNR[23:16] */
	0x1F8A, /* ANTSNR[15:8] */
	0x1F8B, /* ANTSNR[7:0] */
	0x1F8C, /* x x x x ANTFEC[13:8] */
	0x1F8D, /* ANTFEC[7:0] */
	0x1F8E, /* MAXCNT[7:0] */
	0x1F8F, /* SCANCNT[7:0] */
	0x1F91, /* MAXPW[23:16] */
	0x1F92, /* MAXPW[15:8] */
	0x1F93, /* MAXPW[7:0] */
	0x1F95, /* CURPWMSE[23:16] */
	0x1F96, /* CURPWMSE[15:8] */
	0x1F97, /* CURPWMSE[7:0] */
#endif /* SMART_ANT */
	0x211F, /* 1'b1 1'b1 1'b1 CIRQEN x x 1'b0 1'b0 1111xx00 */
	0x212A, /* EQAUTOST */
	0x2122, /* CHFAST[7:0] 01100000 */
	0x212B, /* FFFSTEP_V[3:0] x FBFSTEP_V[2:0] 0001x001 */
	0x212C, /* PHDEROTBWSEL[3:0] 1'b1 1'b1 1'b1 1'b0 10001110 */
	0x212D, /* 1'b1 1'b1 1'b1 1'b1 x x TPIFLOCKS */
	0x2135, /* DYNTRACKFDEQ[3:0] x 1'b0 1'b0 1'b0 1010x000 */
	0x2141, /* TRMODE[1:0] 1'b1 1'b1 1'b0 1'b1 1'b1 1'b1 01110111 */
	0x2162, /* AICCCTRLE */
	0x2173, /* PHNCNFCNT[7:0] 00000100 */
	0x2179, /* 1'b0 1'b0 1'b0 1'b1 x BADSINGLEDYNTRACKFBF[2:0] 0001x001 */
	0x217A, /* 1'b0 1'b0 1'b0 1'b1 x BADSLOWSINGLEDYNTRACKFBF[2:0] 0001x001 */
	0x217E, /* CNFCNTTPIF[7:0] 00001000 */
	0x217F, /* TPERRCNTTPIF[7:0] 00000001 */
	0x2180, /* x x x x x x FBDLYCIR[9:8] */
	0x2181, /* FBDLYCIR[7:0] */
	0x2185, /* MAXPWRMAIN[7:0] */
	0x2191, /* NCOMBDET x x x x x x x */
	0x2199, /* x MAINSTRON */
	0x219A, /* FFFEQSTEPOUT_V[3:0] FBFSTEPOUT_V[2:0] */
	0x21A1, /* x x SNRREF[5:0] */
	0x2845, /* 1'b0 1'b1 x x FFFSTEP_CQS[1:0] FFFCENTERTAP[1:0] 01xx1110 */
	0x2846, /* 1'b0 x 1'b0 1'b1 FBFSTEP_CQS[1:0] 1'b1 1'b0 0x011110 */
	0x2847, /* ENNOSIGDE */
	0x2849, /* 1'b1 1'b1 NOUSENOSI */
	0x284A, /* EQINITWAITTIME[7:0] 01100100 */
	0x3000, /* 1'b1 1'b1 1'b1 x x x 1'b0 RPTRSTM */
	0x3001, /* RPTRSTWAITTIME[7:0] (100msec) 00110010 */
	0x3031, /* FRAMELOC */
	0x3032, /* 1'b1 1'b0 1'b0 1'b0 x x FRAMELOCKMODE_CQS[1:0] 1000xx11 */
	0x30A9, /* VDLOCK_Q FRAMELOCK */
	0x30AA, /* MPEGLOCK */
};

#define numDumpRegs  (sizeof(regtab)/sizeof(regtab[0]))
static u8 regval1[numDumpRegs] = {0, };
static u8 regval2[numDumpRegs] = {0, };

static void lgdt3306a_DumpAllRegs(struct lgdt3306a_state *state)
{
		memset(regval2, 0xff, sizeof(regval2));
		lgdt3306a_DumpRegs(state);
}

static void lgdt3306a_DumpRegs(struct lgdt3306a_state *state)
{
	int i;
	int sav_debug = debug;

	if ((debug & DBG_DUMP) == 0)
		return;
	debug &= ~DBG_REG; /* suppress DBG_REG during reg dump */

	lg_info("\n");

	for (i = 0; i < numDumpRegs; i++) {
		lgdt3306a_read_reg(state, regtab[i], &regval1[i]);
		if (regval1[i] != regval2[i]) {
			lg_info(" %04X = %02X\n", regtab[i], regval1[i]);
				regval2[i] = regval1[i];
		}
	}
	debug = sav_debug;
}
#endif /* DBG_DUMP */



static struct dvb_frontend_ops lgdt3306a_ops = {
	.delsys = { SYS_ATSC, SYS_DVBC_ANNEX_B },
	.info = {
		.name = "LG Electronics LGDT3306A VSB/QAM Frontend",
#if 0
		.type               = FE_ATSC,
#endif
		.frequency_min      = 54000000,
		.frequency_max      = 858000000,
		.frequency_stepsize = 62500,
		.caps = FE_CAN_QAM_64 | FE_CAN_QAM_256 | FE_CAN_8VSB
	},
	.i2c_gate_ctrl        = lgdt3306a_i2c_gate_ctrl,
	.init                 = lgdt3306a_init,
	.sleep                = lgdt3306a_fe_sleep,
	/* if this is set, it overrides the default swzigzag */
	.tune                 = lgdt3306a_tune,
	.set_frontend         = lgdt3306a_set_parameters,
	.get_frontend         = lgdt3306a_get_frontend,
	.get_frontend_algo    = lgdt3306a_get_frontend_algo,
	.get_tune_settings    = lgdt3306a_get_tune_settings,
	.read_status          = lgdt3306a_read_status,
	.read_ber             = lgdt3306a_read_ber,
	.read_signal_strength = lgdt3306a_read_signal_strength,
	.read_snr             = lgdt3306a_read_snr,
	.read_ucblocks        = lgdt3306a_read_ucblocks,
	.release              = lgdt3306a_release,
	.ts_bus_ctrl          = lgdt3306a_ts_bus_ctrl,
	.search               = lgdt3306a_search,
};

MODULE_DESCRIPTION("LG Electronics LGDT3306A ATSC/QAM-B Demodulator Driver");
MODULE_AUTHOR("Fred Richter <frichter@hauppauge.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2");

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
