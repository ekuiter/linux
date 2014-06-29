/*
 * Copyright (c) 1996, 2003 VIA Networking Technologies, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * File: rxtx.c
 *
 * Purpose: handle WMAC/802.3/802.11 rx & tx functions
 *
 * Author: Lyndon Chen
 *
 * Date: May 20, 2003
 *
 * Functions:
 *      s_vGenerateTxParameter - Generate tx dma required parameter.
 *      csBeacon_xmit - beacon tx function
 *      csMgmt_xmit - management tx function
 *      s_uGetDataDuration - get tx data required duration
 *      s_uFillDataHead- fulfill tx data duration header
 *      s_uGetRTSCTSDuration- get rtx/cts required duration
 *      s_uGetRTSCTSRsvTime- get rts/cts reserved time
 *      s_uGetTxRsvTime- get frame reserved time
 *      s_vFillCTSHead- fulfill CTS ctl header
 *      s_vFillFragParameter- Set fragment ctl parameter.
 *      s_vFillRTSHead- fulfill RTS ctl header
 *      vDMA0_tx_80211- tx 802.11 frame via dma0
 *      vGenerateFIFOHeader- Generate tx FIFO ctl header
 *
 * Revision History:
 *
 */

#include "device.h"
#include "rxtx.h"
#include "card.h"
#include "mac.h"
#include "rf.h"
#include "usbpipe.h"

static int          msglevel                = MSG_LEVEL_INFO;

static const u16 wTimeStampOff[2][MAX_RATE] = {
        {384, 288, 226, 209, 54, 43, 37, 31, 28, 25, 24, 23}, // Long Preamble
        {384, 192, 130, 113, 54, 43, 37, 31, 28, 25, 24, 23}, // Short Preamble
    };

static const u16 wFB_Opt0[2][5] = {
        {RATE_12M, RATE_18M, RATE_24M, RATE_36M, RATE_48M}, // fallback_rate0
        {RATE_12M, RATE_12M, RATE_18M, RATE_24M, RATE_36M}, // fallback_rate1
    };
static const u16 wFB_Opt1[2][5] = {
        {RATE_12M, RATE_18M, RATE_24M, RATE_24M, RATE_36M}, // fallback_rate0
        {RATE_6M , RATE_6M,  RATE_12M, RATE_12M, RATE_18M}, // fallback_rate1
    };

#define RTSDUR_BB       0
#define RTSDUR_BA       1
#define RTSDUR_AA       2
#define CTSDUR_BA       3
#define RTSDUR_BA_F0    4
#define RTSDUR_AA_F0    5
#define RTSDUR_BA_F1    6
#define RTSDUR_AA_F1    7
#define CTSDUR_BA_F0    8
#define CTSDUR_BA_F1    9
#define DATADUR_B       10
#define DATADUR_A       11
#define DATADUR_A_F0    12
#define DATADUR_A_F1    13

static struct vnt_usb_send_context *s_vGetFreeContext(struct vnt_private *);

static u16 s_vGenerateTxParameter(struct vnt_usb_send_context *tx_context,
	u8 byPktType, u16 wCurrentRate,	struct vnt_tx_buffer *tx_buffer,
	struct vnt_mic_hdr **mic_hdr, u32 need_mic, u32 cbFrameSize,
	int bNeedACK, struct ethhdr *psEthHeader, bool need_rts);

static unsigned int s_uGetTxRsvTime(struct vnt_private *pDevice, u8 byPktType,
	u32 cbFrameLength, u16 wRate, int bNeedAck);

static __le16 s_uGetRTSCTSRsvTime(struct vnt_private *priv,
	u8 rsv_type, u8 pkt_type, u32 frame_length, u16 current_rate);

static u16 s_vFillCTSHead(struct vnt_usb_send_context *tx_context,
	u8 byPktType, union vnt_tx_data_head *head, u32 cbFrameLength,
	int bNeedAck, u16 wCurrentRate, u8 byFBOption);

static u16 s_vFillRTSHead(struct vnt_usb_send_context *tx_context, u8 byPktType,
	union vnt_tx_data_head *head, u32 cbFrameLength, int bNeedAck,
	struct ethhdr *psEthHeader, u16 wCurrentRate, u8 byFBOption);

static __le16 s_uGetDataDuration(struct vnt_private *pDevice,
	u8 byPktType, int bNeedAck);

static __le16 s_uGetRTSCTSDuration(struct vnt_private *pDevice,
	u8 byDurType, u32 cbFrameLength, u8 byPktType, u16 wRate,
	int bNeedAck, u8 byFBOption);

static struct vnt_usb_send_context
	*s_vGetFreeContext(struct vnt_private *priv)
{
	struct vnt_usb_send_context *context = NULL;
	int ii;

	DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"GetFreeContext()\n");

	for (ii = 0; ii < priv->cbTD; ii++) {
		if (!priv->apTD[ii])
			return NULL;

		context = priv->apTD[ii];
		if (context->in_use == false) {
			context->in_use = true;
			memset(context->data, 0,
					MAX_TOTAL_SIZE_WITH_ALL_HEADERS);

			context->hdr = NULL;

			return context;
		}
	}

	if (ii == priv->cbTD)
		DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"No Free Tx Context\n");

	return NULL;
}

static __le16 vnt_time_stamp_off(struct vnt_private *priv, u16 rate)
{
	return cpu_to_le16(wTimeStampOff[priv->byPreambleType % 2]
							[rate % MAX_RATE]);
}

/*byPktType : PK_TYPE_11A     0
             PK_TYPE_11B     1
             PK_TYPE_11GB    2
             PK_TYPE_11GA    3
*/
static u32 s_uGetTxRsvTime(struct vnt_private *priv, u8 pkt_type,
	u32 frame_length, u16 rate, int need_ack)
{
	u32 data_time, ack_time;

	data_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
							frame_length, rate);

	if (pkt_type == PK_TYPE_11B)
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
					14, (u16)priv->byTopCCKBasicRate);
	else
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
					14, (u16)priv->byTopOFDMBasicRate);

	if (need_ack)
		return data_time + priv->uSIFS + ack_time;

	return data_time;
}

static __le16 vnt_rxtx_rsvtime_le16(struct vnt_private *priv, u8 pkt_type,
	u32 frame_length, u16 rate, int need_ack)
{
	return cpu_to_le16((u16)s_uGetTxRsvTime(priv, pkt_type,
		frame_length, rate, need_ack));
}

//byFreqType: 0=>5GHZ 1=>2.4GHZ
static __le16 s_uGetRTSCTSRsvTime(struct vnt_private *priv,
	u8 rsv_type, u8 pkt_type, u32 frame_length, u16 current_rate)
{
	u32 rrv_time, rts_time, cts_time, ack_time, data_time;

	rrv_time = rts_time = cts_time = ack_time = data_time = 0;

	data_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
						frame_length, current_rate);

	if (rsv_type == 0) {
		rts_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 20, priv->byTopCCKBasicRate);
		cts_time = ack_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 14, priv->byTopCCKBasicRate);
	} else if (rsv_type == 1) {
		rts_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 20, priv->byTopCCKBasicRate);
		cts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopCCKBasicRate);
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopOFDMBasicRate);
	} else if (rsv_type == 2) {
		rts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			20, priv->byTopOFDMBasicRate);
		cts_time = ack_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 14, priv->byTopOFDMBasicRate);
	} else if (rsv_type == 3) {
		cts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopCCKBasicRate);
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopOFDMBasicRate);

		rrv_time = cts_time + ack_time + data_time + 2 * priv->uSIFS;

		return cpu_to_le16((u16)rrv_time);
	}

	rrv_time = rts_time + cts_time + ack_time + data_time + 3 * priv->uSIFS;

	return cpu_to_le16((u16)rrv_time);
}

//byFreqType 0: 5GHz, 1:2.4Ghz
static __le16 s_uGetDataDuration(struct vnt_private *pDevice,
					u8 byPktType, int bNeedAck)
{
	u32 uAckTime = 0;

	if (bNeedAck) {
		if (byPktType == PK_TYPE_11B)
			uAckTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopCCKBasicRate);
		else
			uAckTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopOFDMBasicRate);
		return cpu_to_le16((u16)(pDevice->uSIFS + uAckTime));
	}

	return 0;
}

//byFreqType: 0=>5GHZ 1=>2.4GHZ
static __le16 s_uGetRTSCTSDuration(struct vnt_private *pDevice, u8 byDurType,
	u32 cbFrameLength, u8 byPktType, u16 wRate, int bNeedAck,
	u8 byFBOption)
{
	u32 uCTSTime = 0, uDurTime = 0;

	switch (byDurType) {
	case RTSDUR_BB:
	case RTSDUR_BA:
	case RTSDUR_BA_F0:
	case RTSDUR_BA_F1:
		uCTSTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopCCKBasicRate);
		uDurTime = uCTSTime + 2 * pDevice->uSIFS +
			s_uGetTxRsvTime(pDevice, byPktType,
						cbFrameLength, wRate, bNeedAck);
		break;

	case RTSDUR_AA:
	case RTSDUR_AA_F0:
	case RTSDUR_AA_F1:
		uCTSTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopOFDMBasicRate);
		uDurTime = uCTSTime + 2 * pDevice->uSIFS +
			s_uGetTxRsvTime(pDevice, byPktType,
						cbFrameLength, wRate, bNeedAck);
		break;

	case CTSDUR_BA:
	case CTSDUR_BA_F0:
	case CTSDUR_BA_F1:
		uDurTime = pDevice->uSIFS + s_uGetTxRsvTime(pDevice,
				byPktType, cbFrameLength, wRate, bNeedAck);
		break;

	default:
		break;
	}

	return cpu_to_le16((u16)uDurTime);
}

static u16 vnt_mac_hdr_pos(struct vnt_usb_send_context *tx_context,
	struct ieee80211_hdr *hdr)
{
	u8 *head = tx_context->data + offsetof(struct vnt_tx_buffer, fifo_head);
	u8 *hdr_pos = (u8 *)hdr;

	tx_context->hdr = hdr;
	if (!tx_context->hdr)
		return 0;

	return (u16)(hdr_pos - head);
}

static u16 vnt_rxtx_datahead_g(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_g *buf,
		u32 frame_len, int need_ack)
{

	struct vnt_private *priv = tx_context->priv;
	struct ieee80211_hdr *hdr =
				(struct ieee80211_hdr *)tx_context->skb->data;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);
	vnt_get_phy_field(priv, frame_len, priv->byTopCCKBasicRate,
							PK_TYPE_11B, &buf->b);

	/* Get Duration and TimeStamp */
	if (ieee80211_is_pspoll(hdr->frame_control)) {
		__le16 dur = cpu_to_le16(priv->current_aid | BIT(14) | BIT(15));

		buf->duration_a = dur;
		buf->duration_b = dur;
	} else {
		buf->duration_a = s_uGetDataDuration(priv, pkt_type, need_ack);
		buf->duration_b = s_uGetDataDuration(priv,
							PK_TYPE_11B, need_ack);
	}

	buf->time_stamp_off_a = vnt_time_stamp_off(priv, rate);
	buf->time_stamp_off_b = vnt_time_stamp_off(priv,
					priv->byTopCCKBasicRate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration_a);
}

static u16 vnt_rxtx_datahead_g_fb(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_g_fb *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);

	vnt_get_phy_field(priv, frame_len, priv->byTopCCKBasicRate,
						PK_TYPE_11B, &buf->b);

	/* Get Duration and TimeStamp */
	buf->duration_a = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_b = s_uGetDataDuration(priv, PK_TYPE_11B, need_ack);

	buf->duration_a_f0 = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_a_f1 = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->time_stamp_off_a = vnt_time_stamp_off(priv, rate);
	buf->time_stamp_off_b = vnt_time_stamp_off(priv,
						priv->byTopCCKBasicRate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration_a);
}

static u16 vnt_rxtx_datahead_a_fb(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_a_fb *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);
	/* Get Duration and TimeStampOff */
	buf->duration = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->duration_f0 = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_f1 = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->time_stamp_off = vnt_time_stamp_off(priv, rate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration);
}

static u16 vnt_rxtx_datahead_ab(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_ab *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;
	struct ieee80211_hdr *hdr =
				(struct ieee80211_hdr *)tx_context->skb->data;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->ab);

	/* Get Duration and TimeStampOff */
	if (ieee80211_is_pspoll(hdr->frame_control)) {
		__le16 dur = cpu_to_le16(priv->current_aid | BIT(14) | BIT(15));

		buf->duration = dur;
	} else {
		buf->duration = s_uGetDataDuration(priv, pkt_type, need_ack);
	}

	buf->time_stamp_off = vnt_time_stamp_off(priv, rate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration);
}

static int vnt_fill_ieee80211_rts(struct vnt_usb_send_context *tx_context,
	struct ieee80211_rts *rts, __le16 duration)
{
	struct ieee80211_hdr *hdr =
				(struct ieee80211_hdr *)tx_context->skb->data;

	rts->duration = duration;
	rts->frame_control =
		cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_RTS);

	memcpy(rts->ra, hdr->addr1, ETH_ALEN);
	memcpy(rts->ta, hdr->addr2, ETH_ALEN);

	return 0;
}

static u16 vnt_rxtx_rts_g_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_g *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len, priv->byTopCCKBasicRate,
		PK_TYPE_11B, &buf->b);
	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);

	buf->duration_bb = s_uGetRTSCTSDuration(priv, RTSDUR_BB, frame_len,
		PK_TYPE_11B, priv->byTopCCKBasicRate, need_ack, fb_option);
	buf->duration_aa = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);
	buf->duration_ba = s_uGetRTSCTSDuration(priv, RTSDUR_BA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	vnt_fill_ieee80211_rts(tx_context, &buf->data, buf->duration_aa);

	return vnt_rxtx_datahead_g(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_g_fb_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_g_fb *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len, priv->byTopCCKBasicRate,
		PK_TYPE_11B, &buf->b);
	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);


	buf->duration_bb = s_uGetRTSCTSDuration(priv, RTSDUR_BB, frame_len,
		PK_TYPE_11B, priv->byTopCCKBasicRate, need_ack, fb_option);
	buf->duration_aa = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);
	buf->duration_ba = s_uGetRTSCTSDuration(priv, RTSDUR_BA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);


	buf->rts_duration_ba_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_BA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);
	buf->rts_duration_aa_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);
	buf->rts_duration_ba_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_BA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);
	buf->rts_duration_aa_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);

	vnt_fill_ieee80211_rts(tx_context, &buf->data, buf->duration_aa);

	return vnt_rxtx_datahead_g_fb(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_ab_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_ab *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->ab);

	buf->duration = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	vnt_fill_ieee80211_rts(tx_context, &buf->data, buf->duration);

	return vnt_rxtx_datahead_ab(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_a_fb_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_a_fb *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);

	buf->duration = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	buf->rts_duration_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);

	buf->rts_duration_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);

	vnt_fill_ieee80211_rts(tx_context, &buf->data, buf->duration);

	return vnt_rxtx_datahead_a_fb(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 s_vFillRTSHead(struct vnt_usb_send_context *tx_context, u8 byPktType,
	union vnt_tx_data_head *head, u32 cbFrameLength, int bNeedAck,
	struct ethhdr *psEthHeader, u16 wCurrentRate, u8 byFBOption)
{

	if (!head)
		return 0;

	/* Note: So far RTSHead doesn't appear in ATIM
	*	& Beacom DMA, so we don't need to take them
	*	into account.
	*	Otherwise, we need to modified codes for them.
	*/
	switch (byPktType) {
	case PK_TYPE_11GB:
	case PK_TYPE_11GA:
		if (byFBOption == AUTO_FB_NONE)
			return vnt_rxtx_rts_g_head(tx_context, &head->rts_g,
				psEthHeader, byPktType, cbFrameLength,
				bNeedAck, wCurrentRate, byFBOption);
		else
			return vnt_rxtx_rts_g_fb_head(tx_context,
				&head->rts_g_fb, psEthHeader, byPktType,
				cbFrameLength, bNeedAck, wCurrentRate,
				byFBOption);
		break;
	case PK_TYPE_11A:
		if (byFBOption) {
			return vnt_rxtx_rts_a_fb_head(tx_context,
				&head->rts_a_fb, psEthHeader, byPktType,
				cbFrameLength, bNeedAck, wCurrentRate,
				byFBOption);
			break;
		}
	case PK_TYPE_11B:
		return vnt_rxtx_rts_ab_head(tx_context, &head->rts_ab,
			psEthHeader, byPktType, cbFrameLength,
			bNeedAck, wCurrentRate, byFBOption);
	}

	return 0;
}

static u16 s_vFillCTSHead(struct vnt_usb_send_context *tx_context,
	u8 byPktType, union vnt_tx_data_head *head, u32 cbFrameLength,
	int bNeedAck, u16 wCurrentRate, u8 byFBOption)
{
	struct vnt_private *pDevice = tx_context->priv;
	u32 uCTSFrameLen = 14;

	if (!head)
		return 0;

	if (byFBOption != AUTO_FB_NONE) {
		/* Auto Fall back */
		struct vnt_cts_fb *pBuf = &head->cts_g_fb;
		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, uCTSFrameLen,
			pDevice->byTopCCKBasicRate, PK_TYPE_11B, &pBuf->b);
		pBuf->duration_ba = s_uGetRTSCTSDuration(pDevice, CTSDUR_BA,
			cbFrameLength, byPktType,
			wCurrentRate, bNeedAck, byFBOption);
		/* Get CTSDuration_ba_f0 */
		pBuf->cts_duration_ba_f0 = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA_F0, cbFrameLength, byPktType,
			pDevice->tx_rate_fb0, bNeedAck, byFBOption);
		/* Get CTSDuration_ba_f1 */
		pBuf->cts_duration_ba_f1 = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA_F1, cbFrameLength, byPktType,
			pDevice->tx_rate_fb1, bNeedAck, byFBOption);
		/* Get CTS Frame body */
		pBuf->data.duration = pBuf->duration_ba;
		pBuf->data.frame_control =
			cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_CTS);

		memcpy(pBuf->data.ra, pDevice->abyCurrentNetAddr, ETH_ALEN);

		return vnt_rxtx_datahead_g_fb(tx_context, byPktType,
				wCurrentRate, &pBuf->data_head, cbFrameLength,
				bNeedAck);
	} else {
		struct vnt_cts *pBuf = &head->cts_g;
		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, uCTSFrameLen,
			pDevice->byTopCCKBasicRate, PK_TYPE_11B, &pBuf->b);
		/* Get CTSDuration_ba */
		pBuf->duration_ba = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA, cbFrameLength, byPktType,
			wCurrentRate, bNeedAck, byFBOption);
		/*Get CTS Frame body*/
		pBuf->data.duration = pBuf->duration_ba;
		pBuf->data.frame_control =
			cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_CTS);

		memcpy(pBuf->data.ra, pDevice->abyCurrentNetAddr, ETH_ALEN);

		return vnt_rxtx_datahead_g(tx_context, byPktType, wCurrentRate,
				&pBuf->data_head, cbFrameLength, bNeedAck);
        }

	return 0;
}

/*+
 *
 * Description:
 *      Generate FIFO control for MAC & Baseband controller
 *
 * Parameters:
 *  In:
 *      pDevice         - Pointer to adpater
 *      pTxDataHead     - Transmit Data Buffer
 *      pTxBufHead      - pTxBufHead
 *      pvRrvTime        - pvRrvTime
 *      pvRTS            - RTS Buffer
 *      pCTS            - CTS Buffer
 *      cbFrameSize     - Transmit Data Length (Hdr+Payload+FCS)
 *      bNeedACK        - If need ACK
 *  Out:
 *      none
 *
 * Return Value: none
 *
-*/

static u16 s_vGenerateTxParameter(struct vnt_usb_send_context *tx_context,
	u8 byPktType, u16 wCurrentRate,	struct vnt_tx_buffer *tx_buffer,
	struct vnt_mic_hdr **mic_hdr, u32 need_mic, u32 cbFrameSize,
	int bNeedACK, struct ethhdr *psEthHeader, bool need_rts)
{
	struct vnt_private *pDevice = tx_context->priv;
	struct vnt_tx_fifo_head *pFifoHead = &tx_buffer->fifo_head;
	union vnt_tx_data_head *head = NULL;
	u16 wFifoCtl;
	u8 byFBOption = AUTO_FB_NONE;

	pFifoHead->current_rate = cpu_to_le16(wCurrentRate);
	wFifoCtl = pFifoHead->wFIFOCtl;

	if (wFifoCtl & FIFOCTL_AUTO_FB_0)
		byFBOption = AUTO_FB_0;
	else if (wFifoCtl & FIFOCTL_AUTO_FB_1)
		byFBOption = AUTO_FB_1;

	if (byPktType == PK_TYPE_11GB || byPktType == PK_TYPE_11GA) {
		if (need_rts) {
			struct vnt_rrv_time_rts *pBuf =
					&tx_buffer->tx_head.tx_rts.rts;

			pBuf->rts_rrv_time_aa = s_uGetRTSCTSRsvTime(pDevice, 2,
					byPktType, cbFrameSize, wCurrentRate);
			pBuf->rts_rrv_time_ba = s_uGetRTSCTSRsvTime(pDevice, 1,
					byPktType, cbFrameSize, wCurrentRate);
			pBuf->rts_rrv_time_bb = s_uGetRTSCTSRsvTime(pDevice, 0,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time_a = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);
			pBuf->rrv_time_b = vnt_rxtx_rsvtime_le16(pDevice,
					PK_TYPE_11B, cbFrameSize,
					pDevice->byTopCCKBasicRate, bNeedACK);

			if (need_mic) {
				*mic_hdr = &tx_buffer->
						tx_head.tx_rts.tx.mic.hdr;
				head = &tx_buffer->tx_head.tx_rts.tx.mic.head;
			} else {
				head = &tx_buffer->tx_head.tx_rts.tx.head;
			}

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
					cbFrameSize, bNeedACK, psEthHeader,
						wCurrentRate, byFBOption);

		} else {
			struct vnt_rrv_time_cts *pBuf = &tx_buffer->
							tx_head.tx_cts.cts;

			pBuf->rrv_time_a = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);
			pBuf->rrv_time_b = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize,
					pDevice->byTopCCKBasicRate, bNeedACK);

			pBuf->cts_rrv_time_ba = s_uGetRTSCTSRsvTime(pDevice, 3,
					byPktType, cbFrameSize, wCurrentRate);

			if (need_mic) {
				*mic_hdr = &tx_buffer->
						tx_head.tx_cts.tx.mic.hdr;
				head = &tx_buffer->tx_head.tx_cts.tx.mic.head;
			} else {
				head = &tx_buffer->tx_head.tx_cts.tx.head;
			}

			/* Fill CTS */
			return s_vFillCTSHead(tx_context, byPktType,
				head, cbFrameSize, bNeedACK, wCurrentRate,
					byFBOption);
		}
	} else if (byPktType == PK_TYPE_11A) {
		if (need_mic) {
			*mic_hdr = &tx_buffer->tx_head.tx_ab.tx.mic.hdr;
			head = &tx_buffer->tx_head.tx_ab.tx.mic.head;
		} else {
			head = &tx_buffer->tx_head.tx_ab.tx.head;
		}

		if (need_rts) {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rts_rrv_time = s_uGetRTSCTSRsvTime(pDevice, 2,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
				cbFrameSize, bNeedACK, psEthHeader,
					wCurrentRate, byFBOption);
		} else {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11A, cbFrameSize,
					wCurrentRate, bNeedACK);

			return vnt_rxtx_datahead_a_fb(tx_context, byPktType,
				wCurrentRate, &head->data_head_a_fb,
						cbFrameSize, bNeedACK);
		}
	} else if (byPktType == PK_TYPE_11B) {
		if (need_mic) {
			*mic_hdr = &tx_buffer->tx_head.tx_ab.tx.mic.hdr;
			head = &tx_buffer->tx_head.tx_ab.tx.mic.head;
		} else {
			head = &tx_buffer->tx_head.tx_ab.tx.head;
		}

		if (need_rts) {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rts_rrv_time = s_uGetRTSCTSRsvTime(pDevice, 0,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize, wCurrentRate,
								bNeedACK);

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
				cbFrameSize,
			bNeedACK, psEthHeader, wCurrentRate, byFBOption);
		} else {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize,
					wCurrentRate, bNeedACK);

			return vnt_rxtx_datahead_ab(tx_context, byPktType,
				wCurrentRate, &head->data_head_ab,
					cbFrameSize, bNeedACK);
		}
	}

	return 0;
}

static void vnt_fill_txkey(struct vnt_usb_send_context *tx_context,
	u8 *key_buffer, struct ieee80211_key_conf *tx_key, struct sk_buff *skb,
	u16 payload_len, struct vnt_mic_hdr *mic_hdr)
{
	struct ieee80211_hdr *hdr = tx_context->hdr;
	struct ieee80211_key_seq seq;
	u8 *iv = ((u8 *)hdr + ieee80211_get_hdrlen_from_skb(skb));

	/* strip header and icv len from payload */
	payload_len -= ieee80211_get_hdrlen_from_skb(skb);
	payload_len -= tx_key->icv_len;

	switch (tx_key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
		memcpy(key_buffer, iv, 3);
		memcpy(key_buffer + 3, tx_key->key, tx_key->keylen);

		if (tx_key->keylen == WLAN_KEY_LEN_WEP40) {
			memcpy(key_buffer + 8, iv, 3);
			memcpy(key_buffer + 11,
					tx_key->key, WLAN_KEY_LEN_WEP40);
		}

		break;
	case WLAN_CIPHER_SUITE_TKIP:
		ieee80211_get_tkip_p2k(tx_key, skb, key_buffer);

		break;
	case WLAN_CIPHER_SUITE_CCMP:

		if (!mic_hdr)
			return;

		mic_hdr->id = 0x59;
		mic_hdr->payload_len = cpu_to_be16(payload_len);
		memcpy(mic_hdr->mic_addr2, hdr->addr2, ETH_ALEN);

		ieee80211_get_key_tx_seq(tx_key, &seq);

		memcpy(mic_hdr->ccmp_pn, seq.ccmp.pn, IEEE80211_CCMP_PN_LEN);

		if (ieee80211_has_a4(hdr->frame_control))
			mic_hdr->hlen = cpu_to_be16(28);
		else
			mic_hdr->hlen = cpu_to_be16(22);

		memcpy(mic_hdr->addr1, hdr->addr1, ETH_ALEN);
		memcpy(mic_hdr->addr2, hdr->addr2, ETH_ALEN);
		memcpy(mic_hdr->addr3, hdr->addr3, ETH_ALEN);

		mic_hdr->frame_control = cpu_to_le16(
			le16_to_cpu(hdr->frame_control) & 0xc78f);
		mic_hdr->seq_ctrl = cpu_to_le16(
				le16_to_cpu(hdr->seq_ctrl) & 0xf);

		if (ieee80211_has_a4(hdr->frame_control))
			memcpy(mic_hdr->addr4, hdr->addr4, ETH_ALEN);


		memcpy(key_buffer, tx_key->key, WLAN_KEY_LEN_CCMP);

		break;
	default:
		break;
	}

}

int vnt_tx_packet(struct vnt_private *priv, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_tx_rate *tx_rate = &info->control.rates[0];
	struct ieee80211_rate *rate;
	struct ieee80211_key_conf *tx_key;
	struct ieee80211_hdr *hdr;
	struct vnt_mic_hdr *mic_hdr = NULL;
	struct vnt_tx_buffer *tx_buffer;
	struct vnt_tx_fifo_head *tx_buffer_head;
	struct vnt_usb_send_context *tx_context;
	unsigned long flags;
	u32 frame_size = 0;
	u16 tx_bytes, tx_header_size, tx_body_size, current_rate, duration_id;
	u8 pkt_type, fb_option = AUTO_FB_NONE;
	bool need_rts = false, need_ack = false, is_pspoll = false;
	bool need_mic = false;

	hdr = (struct ieee80211_hdr *)(skb->data);

	rate = ieee80211_get_tx_rate(priv->hw, info);

	current_rate = rate->hw_value;
	if (priv->wCurrentRate != current_rate) {
		priv->wCurrentRate = current_rate;
		bScheduleCommand(priv, WLAN_CMD_SETPOWER, NULL);
	}

	if (current_rate > RATE_11M)
		pkt_type = priv->byPacketType;
	else
		pkt_type = PK_TYPE_11B;

	spin_lock_irqsave(&priv->lock, flags);

	tx_context = s_vGetFreeContext(priv);
	if (!tx_context) {
		dev_dbg(&priv->usb->dev, "%s No free context\n", __func__);
		spin_unlock_irqrestore(&priv->lock, flags);
		return -ENOMEM;
	}

	tx_context->skb = skb;

	spin_unlock_irqrestore(&priv->lock, flags);

	tx_buffer = (struct vnt_tx_buffer *)tx_context->data;
	tx_buffer_head = &tx_buffer->fifo_head;
	tx_body_size = skb->len;

	frame_size = tx_body_size + 4;

	/*Set fifo controls */
	if (pkt_type == PK_TYPE_11A)
		tx_buffer_head->wFIFOCtl = 0;
	else if (pkt_type == PK_TYPE_11B)
		tx_buffer_head->wFIFOCtl = FIFOCTL_11B;
	else if (pkt_type == PK_TYPE_11GB)
		tx_buffer_head->wFIFOCtl = FIFOCTL_11GB;
	else if (pkt_type == PK_TYPE_11GA)
		tx_buffer_head->wFIFOCtl = FIFOCTL_11GA;

	if (!ieee80211_is_data(hdr->frame_control)) {
		tx_buffer_head->wFIFOCtl |= (FIFOCTL_GENINT |
			FIFOCTL_ISDMA0);
		tx_buffer_head->wFIFOCtl |= FIFOCTL_TMOEN;

		tx_buffer_head->time_stamp =
			cpu_to_le16(DEFAULT_MGN_LIFETIME_RES_64us);
	} else {
		tx_buffer_head->time_stamp =
			cpu_to_le16(DEFAULT_MSDU_LIFETIME_RES_64us);
	}

	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK)) {
		tx_buffer_head->wFIFOCtl |= FIFOCTL_NEEDACK;
		need_ack = true;
	}

	if (ieee80211_has_retry(hdr->frame_control))
		tx_buffer_head->wFIFOCtl |= FIFOCTL_LRETRY;

	if (tx_rate->flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE)
		priv->byPreambleType = PREAMBLE_SHORT;
	else
		priv->byPreambleType = PREAMBLE_LONG;

	if (tx_rate->flags & IEEE80211_TX_RC_USE_RTS_CTS) {
		need_rts = true;
		tx_buffer_head->wFIFOCtl |= FIFOCTL_RTS;
	}

	if (ieee80211_has_a4(hdr->frame_control))
		tx_buffer_head->wFIFOCtl |= FIFOCTL_LHEAD;

	if (info->flags & IEEE80211_TX_CTL_NO_PS_BUFFER)
		is_pspoll = true;

	tx_buffer_head->frag_ctl =
			cpu_to_le16(ieee80211_get_hdrlen_from_skb(skb) << 10);

	if (info->control.hw_key) {
		tx_key = info->control.hw_key;
		switch (info->control.hw_key->cipher) {
		case WLAN_CIPHER_SUITE_WEP40:
		case WLAN_CIPHER_SUITE_WEP104:
			tx_buffer_head->frag_ctl |= cpu_to_le16(FRAGCTL_LEGACY);
			break;
		case WLAN_CIPHER_SUITE_TKIP:
			tx_buffer_head->frag_ctl |= cpu_to_le16(FRAGCTL_TKIP);
			break;
		case WLAN_CIPHER_SUITE_CCMP:
			tx_buffer_head->frag_ctl |= cpu_to_le16(FRAGCTL_AES);
			need_mic = true;
		default:
			break;
		}
		frame_size += tx_key->icv_len;
	}

	/* legacy rates TODO use ieee80211_tx_rate */
	if (current_rate >= RATE_18M && ieee80211_is_data(hdr->frame_control)) {
		if (priv->byAutoFBCtrl == AUTO_FB_0) {
			tx_buffer_head->wFIFOCtl |= FIFOCTL_AUTO_FB_0;

			priv->tx_rate_fb0 =
				wFB_Opt0[FB_RATE0][current_rate - RATE_18M];
			priv->tx_rate_fb1 =
				wFB_Opt0[FB_RATE1][current_rate - RATE_18M];

			fb_option = AUTO_FB_0;
		} else if (priv->byAutoFBCtrl == AUTO_FB_1) {
			tx_buffer_head->wFIFOCtl |= FIFOCTL_AUTO_FB_1;

			priv->tx_rate_fb0 =
				wFB_Opt1[FB_RATE0][current_rate - RATE_18M];
			priv->tx_rate_fb1 =
				wFB_Opt1[FB_RATE1][current_rate - RATE_18M];

			fb_option = AUTO_FB_1;
		}
	}

	duration_id = s_vGenerateTxParameter(tx_context, pkt_type, current_rate,
				tx_buffer, &mic_hdr, need_mic, frame_size,
						need_ack, NULL, need_rts);

	tx_header_size = tx_context->tx_hdr_size;
	if (!tx_header_size) {
		tx_context->in_use = false;
		return -ENOMEM;
	}

	tx_buffer_head->frag_ctl |= cpu_to_le16(FRAGCTL_NONFRAG);

	tx_bytes = tx_header_size + tx_body_size;

	memcpy(tx_context->hdr, skb->data, tx_body_size);

	hdr->duration_id = cpu_to_le16(duration_id);

	if (info->control.hw_key) {
		tx_key = info->control.hw_key;
		if (tx_key->keylen > 0)
			vnt_fill_txkey(tx_context, tx_buffer_head->tx_key,
				tx_key, skb, tx_body_size, mic_hdr);
	}

	priv->wSeqCounter = (le16_to_cpu(hdr->seq_ctrl) &
						IEEE80211_SCTL_SEQ) >> 4;

	tx_buffer->tx_byte_count = cpu_to_le16(tx_bytes);
	tx_buffer->byPKTNO = (u8)(((current_rate << 4) & 0xf0) |
		(priv->wSeqCounter & 0xf));
	tx_buffer->byType = 0x00;

	tx_bytes += 4;

	tx_context->type = CONTEXT_DATA_PACKET;
	tx_context->buf_len = tx_bytes;

	spin_lock_irqsave(&priv->lock, flags);

	if (PIPEnsSendBulkOut(priv, tx_context) != STATUS_PENDING) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EIO;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int vnt_beacon_xmit(struct vnt_private *priv,
	struct sk_buff *skb)
{
	struct vnt_beacon_buffer *beacon_buffer;
	struct vnt_tx_short_buf_head *short_head;
	struct ieee80211_tx_info *info;
	struct vnt_usb_send_context *context;
	struct ieee80211_mgmt *mgmt_hdr;
	unsigned long flags;
	u32 frame_size = skb->len + 4;
	u16 current_rate, count;

	spin_lock_irqsave(&priv->lock, flags);

	context = s_vGetFreeContext(priv);
	if (!context) {
		dev_dbg(&priv->usb->dev, "%s No free context!\n", __func__);
		spin_unlock_irqrestore(&priv->lock, flags);
		return -ENOMEM;
	}

	context->skb = skb;

	spin_unlock_irqrestore(&priv->lock, flags);

	beacon_buffer = (struct vnt_beacon_buffer *)&context->data[0];
	short_head = &beacon_buffer->short_head;

	if (priv->byBBType == BB_TYPE_11A) {
		current_rate = RATE_6M;

		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(priv, frame_size, current_rate,
			PK_TYPE_11A, &short_head->ab);

		/* Get Duration and TimeStampOff */
		short_head->duration = s_uGetDataDuration(priv,
							PK_TYPE_11A, false);
		short_head->time_stamp_off =
				vnt_time_stamp_off(priv, current_rate);
	} else {
		current_rate = RATE_1M;
		short_head->fifo_ctl |= FIFOCTL_11B;

		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(priv, frame_size, current_rate,
					PK_TYPE_11B, &short_head->ab);

		/* Get Duration and TimeStampOff */
		short_head->duration = s_uGetDataDuration(priv,
						PK_TYPE_11B, false);
		short_head->time_stamp_off =
			vnt_time_stamp_off(priv, current_rate);
	}

	/* Generate Beacon Header */
	mgmt_hdr = &beacon_buffer->mgmt_hdr;
	memcpy(mgmt_hdr, skb->data, skb->len);

	/* time stamp always 0 */
	mgmt_hdr->u.beacon.timestamp = 0;

	info = IEEE80211_SKB_CB(skb);
	if (info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ) {
		struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)mgmt_hdr;
		hdr->duration_id = 0;
		hdr->seq_ctrl = cpu_to_le16(priv->wSeqCounter << 4);
	}

	priv->wSeqCounter++;
	if (priv->wSeqCounter > 0x0fff)
		priv->wSeqCounter = 0;

	count = sizeof(struct vnt_tx_short_buf_head) + skb->len;

	beacon_buffer->tx_byte_count = cpu_to_le16(count);
	beacon_buffer->byPKTNO = (u8)(((current_rate << 4) & 0xf0) |
				((priv->wSeqCounter - 1) & 0x000f));
	beacon_buffer->byType = 0x01;

	context->type = CONTEXT_BEACON_PACKET;
	context->buf_len = count + 4; /* USB header */

	spin_lock_irqsave(&priv->lock, flags);

	if (PIPEnsSendBulkOut(priv, context) != STATUS_PENDING)
		ieee80211_free_txskb(priv->hw, context->skb);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

int vnt_beacon_make(struct vnt_private *priv, struct ieee80211_vif *vif)
{
	struct sk_buff *beacon;

	beacon = ieee80211_beacon_get(priv->hw, vif);
	if (!beacon)
		return -ENOMEM;

	if (vnt_beacon_xmit(priv, beacon)) {
		ieee80211_free_txskb(priv->hw, beacon);
		return -ENODEV;
	}

	return 0;
}

int vnt_beacon_enable(struct vnt_private *priv, struct ieee80211_vif *vif,
	struct ieee80211_bss_conf *conf)
{
	int ret;

	vnt_mac_reg_bits_off(priv, MAC_REG_TCR, TCR_AUTOBCNTX);

	vnt_mac_reg_bits_off(priv, MAC_REG_TFTCTL, TFTCTL_TSFCNTREN);

	vnt_mac_set_beacon_interval(priv, conf->beacon_int);

	vnt_clear_current_tsf(priv);

	vnt_mac_reg_bits_on(priv, MAC_REG_TFTCTL, TFTCTL_TSFCNTREN);

	vnt_reset_next_tbtt(priv, conf->beacon_int);

	ret = vnt_beacon_make(priv, vif);

	return ret;
}
