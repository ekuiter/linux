/******************************************************************************
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2008 - 2011 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called LICENSE.GPL.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 *****************************************************************************/
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>

#include "iwl-dev.h"
#include "iwl-core.h"
#include "iwl-io.h"
#include "iwl-helpers.h"
#include "iwl-4965-hw.h"
#include "iwl-4965.h"
#include "iwl-sta.h"

void il4965_check_abort_status(struct il_priv *il,
			    u8 frame_count, u32 status)
{
	if (frame_count == 1 && status == TX_STATUS_FAIL_RFKILL_FLUSH) {
		IL_ERR("Tx flush command to flush out all frames\n");
		if (!test_bit(STATUS_EXIT_PENDING, &il->status))
			queue_work(il->workqueue, &il->tx_flush);
	}
}

/*
 * EEPROM
 */
struct il_mod_params il4965_mod_params = {
	.amsdu_size_8K = 1,
	.restart_fw = 1,
	/* the rest are 0 by default */
};

void il4965_rx_queue_reset(struct il_priv *il, struct il_rx_queue *rxq)
{
	unsigned long flags;
	int i;
	spin_lock_irqsave(&rxq->lock, flags);
	INIT_LIST_HEAD(&rxq->rx_free);
	INIT_LIST_HEAD(&rxq->rx_used);
	/* Fill the rx_used queue with _all_ of the Rx buffers */
	for (i = 0; i < RX_FREE_BUFFERS + RX_QUEUE_SIZE; i++) {
		/* In the reset function, these buffers may have been allocated
		 * to an SKB, so we need to unmap and free potential storage */
		if (rxq->pool[i].page != NULL) {
			pci_unmap_page(il->pci_dev, rxq->pool[i].page_dma,
				PAGE_SIZE << il->hw_params.rx_page_order,
				PCI_DMA_FROMDEVICE);
			__il_free_pages(il, rxq->pool[i].page);
			rxq->pool[i].page = NULL;
		}
		list_add_tail(&rxq->pool[i].list, &rxq->rx_used);
	}

	for (i = 0; i < RX_QUEUE_SIZE; i++)
		rxq->queue[i] = NULL;

	/* Set us so that we have processed and used all buffers, but have
	 * not restocked the Rx queue with fresh buffers */
	rxq->read = rxq->write = 0;
	rxq->write_actual = 0;
	rxq->free_count = 0;
	spin_unlock_irqrestore(&rxq->lock, flags);
}

int il4965_rx_init(struct il_priv *il, struct il_rx_queue *rxq)
{
	u32 rb_size;
	const u32 rfdnlog = RX_QUEUE_SIZE_LOG; /* 256 RBDs */
	u32 rb_timeout = 0;

	if (il->cfg->mod_params->amsdu_size_8K)
		rb_size = FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_8K;
	else
		rb_size = FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K;

	/* Stop Rx DMA */
	il_wr(il, FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

	/* Reset driver's Rx queue write index */
	il_wr(il, FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);

	/* Tell device where to find RBD circular buffer in DRAM */
	il_wr(il, FH_RSCSR_CHNL0_RBDCB_BASE_REG,
			   (u32)(rxq->bd_dma >> 8));

	/* Tell device where in DRAM to update its Rx status */
	il_wr(il, FH_RSCSR_CHNL0_STTS_WPTR_REG,
			   rxq->rb_stts_dma >> 4);

	/* Enable Rx DMA
	 * Direct rx interrupts to hosts
	 * Rx buffer size 4 or 8k
	 * RB timeout 0x10
	 * 256 RBDs
	 */
	il_wr(il, FH_MEM_RCSR_CHNL0_CONFIG_REG,
			   FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL |
			   FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
			   FH_RCSR_CHNL0_RX_CONFIG_SINGLE_FRAME_MSK |
			   rb_size|
			   (rb_timeout << FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS)|
			   (rfdnlog << FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS));

	/* Set interrupt coalescing timer to default (2048 usecs) */
	il_write8(il, CSR_INT_COALESCING, IL_HOST_INT_TIMEOUT_DEF);

	return 0;
}

static void il4965_set_pwr_vmain(struct il_priv *il)
{
/*
 * (for documentation purposes)
 * to set power to V_AUX, do:

		if (pci_pme_capable(il->pci_dev, PCI_D3cold))
			il_set_bits_mask_prph(il, APMG_PS_CTRL_REG,
					       APMG_PS_CTRL_VAL_PWR_SRC_VAUX,
					       ~APMG_PS_CTRL_MSK_PWR_SRC);
 */

	il_set_bits_mask_prph(il, APMG_PS_CTRL_REG,
			       APMG_PS_CTRL_VAL_PWR_SRC_VMAIN,
			       ~APMG_PS_CTRL_MSK_PWR_SRC);
}

int il4965_hw_nic_init(struct il_priv *il)
{
	unsigned long flags;
	struct il_rx_queue *rxq = &il->rxq;
	int ret;

	/* nic_init */
	spin_lock_irqsave(&il->lock, flags);
	il->cfg->ops->lib->apm_ops.init(il);

	/* Set interrupt coalescing calibration timer to default (512 usecs) */
	il_write8(il, CSR_INT_COALESCING, IL_HOST_INT_CALIB_TIMEOUT_DEF);

	spin_unlock_irqrestore(&il->lock, flags);

	il4965_set_pwr_vmain(il);

	il->cfg->ops->lib->apm_ops.config(il);

	/* Allocate the RX queue, or reset if it is already allocated */
	if (!rxq->bd) {
		ret = il_rx_queue_alloc(il);
		if (ret) {
			IL_ERR("Unable to initialize Rx queue\n");
			return -ENOMEM;
		}
	} else
		il4965_rx_queue_reset(il, rxq);

	il4965_rx_replenish(il);

	il4965_rx_init(il, rxq);

	spin_lock_irqsave(&il->lock, flags);

	rxq->need_update = 1;
	il_rx_queue_update_write_ptr(il, rxq);

	spin_unlock_irqrestore(&il->lock, flags);

	/* Allocate or reset and init all Tx and Command queues */
	if (!il->txq) {
		ret = il4965_txq_ctx_alloc(il);
		if (ret)
			return ret;
	} else
		il4965_txq_ctx_reset(il);

	set_bit(STATUS_INIT, &il->status);

	return 0;
}

/**
 * il4965_dma_addr2rbd_ptr - convert a DMA address to a uCode read buffer ptr
 */
static inline __le32 il4965_dma_addr2rbd_ptr(struct il_priv *il,
					  dma_addr_t dma_addr)
{
	return cpu_to_le32((u32)(dma_addr >> 8));
}

/**
 * il4965_rx_queue_restock - refill RX queue from pre-allocated pool
 *
 * If there are slots in the RX queue that need to be restocked,
 * and we have free pre-allocated buffers, fill the ranks as much
 * as we can, pulling from rx_free.
 *
 * This moves the 'write' index forward to catch up with 'processed', and
 * also updates the memory address in the firmware to reference the new
 * target buffer.
 */
void il4965_rx_queue_restock(struct il_priv *il)
{
	struct il_rx_queue *rxq = &il->rxq;
	struct list_head *element;
	struct il_rx_buf *rxb;
	unsigned long flags;

	spin_lock_irqsave(&rxq->lock, flags);
	while (il_rx_queue_space(rxq) > 0 && rxq->free_count) {
		/* The overwritten rxb must be a used one */
		rxb = rxq->queue[rxq->write];
		BUG_ON(rxb && rxb->page);

		/* Get next free Rx buffer, remove from free list */
		element = rxq->rx_free.next;
		rxb = list_entry(element, struct il_rx_buf, list);
		list_del(element);

		/* Point to Rx buffer via next RBD in circular buffer */
		rxq->bd[rxq->write] = il4965_dma_addr2rbd_ptr(il,
							      rxb->page_dma);
		rxq->queue[rxq->write] = rxb;
		rxq->write = (rxq->write + 1) & RX_QUEUE_MASK;
		rxq->free_count--;
	}
	spin_unlock_irqrestore(&rxq->lock, flags);
	/* If the pre-allocated buffer pool is dropping low, schedule to
	 * refill it */
	if (rxq->free_count <= RX_LOW_WATERMARK)
		queue_work(il->workqueue, &il->rx_replenish);


	/* If we've added more space for the firmware to place data, tell it.
	 * Increment device's write pointer in multiples of 8. */
	if (rxq->write_actual != (rxq->write & ~0x7)) {
		spin_lock_irqsave(&rxq->lock, flags);
		rxq->need_update = 1;
		spin_unlock_irqrestore(&rxq->lock, flags);
		il_rx_queue_update_write_ptr(il, rxq);
	}
}

/**
 * il4965_rx_replenish - Move all used packet from rx_used to rx_free
 *
 * When moving to rx_free an SKB is allocated for the slot.
 *
 * Also restock the Rx queue via il_rx_queue_restock.
 * This is called as a scheduled work item (except for during initialization)
 */
static void il4965_rx_allocate(struct il_priv *il, gfp_t priority)
{
	struct il_rx_queue *rxq = &il->rxq;
	struct list_head *element;
	struct il_rx_buf *rxb;
	struct page *page;
	unsigned long flags;
	gfp_t gfp_mask = priority;

	while (1) {
		spin_lock_irqsave(&rxq->lock, flags);
		if (list_empty(&rxq->rx_used)) {
			spin_unlock_irqrestore(&rxq->lock, flags);
			return;
		}
		spin_unlock_irqrestore(&rxq->lock, flags);

		if (rxq->free_count > RX_LOW_WATERMARK)
			gfp_mask |= __GFP_NOWARN;

		if (il->hw_params.rx_page_order > 0)
			gfp_mask |= __GFP_COMP;

		/* Alloc a new receive buffer */
		page = alloc_pages(gfp_mask, il->hw_params.rx_page_order);
		if (!page) {
			if (net_ratelimit())
				D_INFO("alloc_pages failed, "
					       "order: %d\n",
					       il->hw_params.rx_page_order);

			if (rxq->free_count <= RX_LOW_WATERMARK &&
			    net_ratelimit())
				IL_ERR(
					"Failed to alloc_pages with %s. "
					"Only %u free buffers remaining.\n",
					 priority == GFP_ATOMIC ?
						 "GFP_ATOMIC" : "GFP_KERNEL",
					 rxq->free_count);
			/* We don't reschedule replenish work here -- we will
			 * call the restock method and if it still needs
			 * more buffers it will schedule replenish */
			return;
		}

		spin_lock_irqsave(&rxq->lock, flags);

		if (list_empty(&rxq->rx_used)) {
			spin_unlock_irqrestore(&rxq->lock, flags);
			__free_pages(page, il->hw_params.rx_page_order);
			return;
		}
		element = rxq->rx_used.next;
		rxb = list_entry(element, struct il_rx_buf, list);
		list_del(element);

		spin_unlock_irqrestore(&rxq->lock, flags);

		BUG_ON(rxb->page);
		rxb->page = page;
		/* Get physical address of the RB */
		rxb->page_dma = pci_map_page(il->pci_dev, page, 0,
				PAGE_SIZE << il->hw_params.rx_page_order,
				PCI_DMA_FROMDEVICE);
		/* dma address must be no more than 36 bits */
		BUG_ON(rxb->page_dma & ~DMA_BIT_MASK(36));
		/* and also 256 byte aligned! */
		BUG_ON(rxb->page_dma & DMA_BIT_MASK(8));

		spin_lock_irqsave(&rxq->lock, flags);

		list_add_tail(&rxb->list, &rxq->rx_free);
		rxq->free_count++;
		il->alloc_rxb_page++;

		spin_unlock_irqrestore(&rxq->lock, flags);
	}
}

void il4965_rx_replenish(struct il_priv *il)
{
	unsigned long flags;

	il4965_rx_allocate(il, GFP_KERNEL);

	spin_lock_irqsave(&il->lock, flags);
	il4965_rx_queue_restock(il);
	spin_unlock_irqrestore(&il->lock, flags);
}

void il4965_rx_replenish_now(struct il_priv *il)
{
	il4965_rx_allocate(il, GFP_ATOMIC);

	il4965_rx_queue_restock(il);
}

/* Assumes that the skb field of the buffers in 'pool' is kept accurate.
 * If an SKB has been detached, the POOL needs to have its SKB set to NULL
 * This free routine walks the list of POOL entries and if SKB is set to
 * non NULL it is unmapped and freed
 */
void il4965_rx_queue_free(struct il_priv *il, struct il_rx_queue *rxq)
{
	int i;
	for (i = 0; i < RX_QUEUE_SIZE + RX_FREE_BUFFERS; i++) {
		if (rxq->pool[i].page != NULL) {
			pci_unmap_page(il->pci_dev, rxq->pool[i].page_dma,
				PAGE_SIZE << il->hw_params.rx_page_order,
				PCI_DMA_FROMDEVICE);
			__il_free_pages(il, rxq->pool[i].page);
			rxq->pool[i].page = NULL;
		}
	}

	dma_free_coherent(&il->pci_dev->dev, 4 * RX_QUEUE_SIZE, rxq->bd,
			  rxq->bd_dma);
	dma_free_coherent(&il->pci_dev->dev, sizeof(struct il_rb_status),
			  rxq->rb_stts, rxq->rb_stts_dma);
	rxq->bd = NULL;
	rxq->rb_stts  = NULL;
}

int il4965_rxq_stop(struct il_priv *il)
{

	/* stop Rx DMA */
	il_wr(il, FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
	il_poll_bit(il, FH_MEM_RSSR_RX_STATUS_REG,
			    FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE, 1000);

	return 0;
}

int il4965_hwrate_to_mac80211_idx(u32 rate_n_flags, enum ieee80211_band band)
{
	int idx = 0;
	int band_offset = 0;

	/* HT rate format: mac80211 wants an MCS number, which is just LSB */
	if (rate_n_flags & RATE_MCS_HT_MSK) {
		idx = (rate_n_flags & 0xff);
		return idx;
	/* Legacy rate format, search for match in table */
	} else {
		if (band == IEEE80211_BAND_5GHZ)
			band_offset = IL_FIRST_OFDM_RATE;
		for (idx = band_offset; idx < IL_RATE_COUNT_LEGACY; idx++)
			if (il_rates[idx].plcp == (rate_n_flags & 0xFF))
				return idx - band_offset;
	}

	return -1;
}

static int il4965_calc_rssi(struct il_priv *il,
			     struct il_rx_phy_res *rx_resp)
{
	/* data from PHY/DSP regarding signal strength, etc.,
	 *   contents are always there, not configurable by host.  */
	struct il4965_rx_non_cfg_phy *ncphy =
	    (struct il4965_rx_non_cfg_phy *)rx_resp->non_cfg_phy_buf;
	u32 agc = (le16_to_cpu(ncphy->agc_info) & IL49_AGC_DB_MASK)
			>> IL49_AGC_DB_POS;

	u32 valid_antennae =
	    (le16_to_cpu(rx_resp->phy_flags) & IL49_RX_PHY_FLAGS_ANTENNAE_MASK)
			>> IL49_RX_PHY_FLAGS_ANTENNAE_OFFSET;
	u8 max_rssi = 0;
	u32 i;

	/* Find max rssi among 3 possible receivers.
	 * These values are measured by the digital signal processor (DSP).
	 * They should stay fairly constant even as the signal strength varies,
	 *   if the radio's automatic gain control (AGC) is working right.
	 * AGC value (see below) will provide the "interesting" info. */
	for (i = 0; i < 3; i++)
		if (valid_antennae & (1 << i))
			max_rssi = max(ncphy->rssi_info[i << 1], max_rssi);

	D_STATS("Rssi In A %d B %d C %d Max %d AGC dB %d\n",
		ncphy->rssi_info[0], ncphy->rssi_info[2], ncphy->rssi_info[4],
		max_rssi, agc);

	/* dBm = max_rssi dB - agc dB - constant.
	 * Higher AGC (higher radio gain) means lower signal. */
	return max_rssi - agc - IL4965_RSSI_OFFSET;
}


static u32 il4965_translate_rx_status(struct il_priv *il, u32 decrypt_in)
{
	u32 decrypt_out = 0;

	if ((decrypt_in & RX_RES_STATUS_STATION_FOUND) ==
					RX_RES_STATUS_STATION_FOUND)
		decrypt_out |= (RX_RES_STATUS_STATION_FOUND |
				RX_RES_STATUS_NO_STATION_INFO_MISMATCH);

	decrypt_out |= (decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK);

	/* packet was not encrypted */
	if ((decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) ==
					RX_RES_STATUS_SEC_TYPE_NONE)
		return decrypt_out;

	/* packet was encrypted with unknown alg */
	if ((decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) ==
					RX_RES_STATUS_SEC_TYPE_ERR)
		return decrypt_out;

	/* decryption was not done in HW */
	if ((decrypt_in & RX_MPDU_RES_STATUS_DEC_DONE_MSK) !=
					RX_MPDU_RES_STATUS_DEC_DONE_MSK)
		return decrypt_out;

	switch (decrypt_in & RX_RES_STATUS_SEC_TYPE_MSK) {

	case RX_RES_STATUS_SEC_TYPE_CCMP:
		/* alg is CCM: check MIC only */
		if (!(decrypt_in & RX_MPDU_RES_STATUS_MIC_OK))
			/* Bad MIC */
			decrypt_out |= RX_RES_STATUS_BAD_ICV_MIC;
		else
			decrypt_out |= RX_RES_STATUS_DECRYPT_OK;

		break;

	case RX_RES_STATUS_SEC_TYPE_TKIP:
		if (!(decrypt_in & RX_MPDU_RES_STATUS_TTAK_OK)) {
			/* Bad TTAK */
			decrypt_out |= RX_RES_STATUS_BAD_KEY_TTAK;
			break;
		}
		/* fall through if TTAK OK */
	default:
		if (!(decrypt_in & RX_MPDU_RES_STATUS_ICV_OK))
			decrypt_out |= RX_RES_STATUS_BAD_ICV_MIC;
		else
			decrypt_out |= RX_RES_STATUS_DECRYPT_OK;
		break;
	}

	D_RX("decrypt_in:0x%x  decrypt_out = 0x%x\n",
					decrypt_in, decrypt_out);

	return decrypt_out;
}

static void il4965_pass_packet_to_mac80211(struct il_priv *il,
					struct ieee80211_hdr *hdr,
					u16 len,
					u32 ampdu_status,
					struct il_rx_buf *rxb,
					struct ieee80211_rx_status *stats)
{
	struct sk_buff *skb;
	__le16 fc = hdr->frame_control;

	/* We only process data packets if the interface is open */
	if (unlikely(!il->is_open)) {
		D_DROP(
		    "Dropping packet while interface is not open.\n");
		return;
	}

	/* In case of HW accelerated crypto and bad decryption, drop */
	if (!il->cfg->mod_params->sw_crypto &&
	    il_set_decrypted_flag(il, hdr, ampdu_status, stats))
		return;

	skb = dev_alloc_skb(128);
	if (!skb) {
		IL_ERR("dev_alloc_skb failed\n");
		return;
	}

	skb_add_rx_frag(skb, 0, rxb->page, (void *)hdr - rxb_addr(rxb), len);

	il_update_stats(il, false, fc, len);
	memcpy(IEEE80211_SKB_RXCB(skb), stats, sizeof(*stats));

	ieee80211_rx(il->hw, skb);
	il->alloc_rxb_page--;
	rxb->page = NULL;
}

/* Called for REPLY_RX (legacy ABG frames), or
 * REPLY_RX_MPDU_CMD (HT high-throughput N frames). */
void il4965_rx_reply_rx(struct il_priv *il,
				struct il_rx_buf *rxb)
{
	struct ieee80211_hdr *header;
	struct ieee80211_rx_status rx_status;
	struct il_rx_pkt *pkt = rxb_addr(rxb);
	struct il_rx_phy_res *phy_res;
	__le32 rx_pkt_status;
	struct il_rx_mpdu_res_start *amsdu;
	u32 len;
	u32 ampdu_status;
	u32 rate_n_flags;

	/**
	 * REPLY_RX and REPLY_RX_MPDU_CMD are handled differently.
	 *	REPLY_RX: physical layer info is in this buffer
	 *	REPLY_RX_MPDU_CMD: physical layer info was sent in separate
	 *		command and cached in il->last_phy_res
	 *
	 * Here we set up local variables depending on which command is
	 * received.
	 */
	if (pkt->hdr.cmd == REPLY_RX) {
		phy_res = (struct il_rx_phy_res *)pkt->u.raw;
		header = (struct ieee80211_hdr *)(pkt->u.raw + sizeof(*phy_res)
				+ phy_res->cfg_phy_cnt);

		len = le16_to_cpu(phy_res->byte_count);
		rx_pkt_status = *(__le32 *)(pkt->u.raw + sizeof(*phy_res) +
				phy_res->cfg_phy_cnt + len);
		ampdu_status = le32_to_cpu(rx_pkt_status);
	} else {
		if (!il->_4965.last_phy_res_valid) {
			IL_ERR("MPDU frame without cached PHY data\n");
			return;
		}
		phy_res = &il->_4965.last_phy_res;
		amsdu = (struct il_rx_mpdu_res_start *)pkt->u.raw;
		header = (struct ieee80211_hdr *)(pkt->u.raw + sizeof(*amsdu));
		len = le16_to_cpu(amsdu->byte_count);
		rx_pkt_status = *(__le32 *)(pkt->u.raw + sizeof(*amsdu) + len);
		ampdu_status = il4965_translate_rx_status(il,
				le32_to_cpu(rx_pkt_status));
	}

	if ((unlikely(phy_res->cfg_phy_cnt > 20))) {
		D_DROP("dsp size out of range [0,20]: %d/n",
				phy_res->cfg_phy_cnt);
		return;
	}

	if (!(rx_pkt_status & RX_RES_STATUS_NO_CRC32_ERROR) ||
	    !(rx_pkt_status & RX_RES_STATUS_NO_RXE_OVERFLOW)) {
		D_RX("Bad CRC or FIFO: 0x%08X.\n",
				le32_to_cpu(rx_pkt_status));
		return;
	}

	/* This will be used in several places later */
	rate_n_flags = le32_to_cpu(phy_res->rate_n_flags);

	/* rx_status carries information about the packet to mac80211 */
	rx_status.mactime = le64_to_cpu(phy_res->timestamp);
	rx_status.band = (phy_res->phy_flags & RX_RES_PHY_FLAGS_BAND_24_MSK) ?
				IEEE80211_BAND_2GHZ : IEEE80211_BAND_5GHZ;
	rx_status.freq =
		ieee80211_channel_to_frequency(le16_to_cpu(phy_res->channel),
							rx_status.band);
	rx_status.rate_idx =
		il4965_hwrate_to_mac80211_idx(rate_n_flags, rx_status.band);
	rx_status.flag = 0;

	/* TSF isn't reliable. In order to allow smooth user experience,
	 * this W/A doesn't propagate it to the mac80211 */
	/*rx_status.flag |= RX_FLAG_MACTIME_MPDU;*/

	il->ucode_beacon_time = le32_to_cpu(phy_res->beacon_time_stamp);

	/* Find max signal strength (dBm) among 3 antenna/receiver chains */
	rx_status.signal = il4965_calc_rssi(il, phy_res);

	il_dbg_log_rx_data_frame(il, len, header);
	D_STATS("Rssi %d, TSF %llu\n",
		rx_status.signal, (unsigned long long)rx_status.mactime);

	/*
	 * "antenna number"
	 *
	 * It seems that the antenna field in the phy flags value
	 * is actually a bit field. This is undefined by radiotap,
	 * it wants an actual antenna number but I always get "7"
	 * for most legacy frames I receive indicating that the
	 * same frame was received on all three RX chains.
	 *
	 * I think this field should be removed in favor of a
	 * new 802.11n radiotap field "RX chains" that is defined
	 * as a bitmask.
	 */
	rx_status.antenna =
		(le16_to_cpu(phy_res->phy_flags) & RX_RES_PHY_FLAGS_ANTENNA_MSK)
		>> RX_RES_PHY_FLAGS_ANTENNA_POS;

	/* set the preamble flag if appropriate */
	if (phy_res->phy_flags & RX_RES_PHY_FLAGS_SHORT_PREAMBLE_MSK)
		rx_status.flag |= RX_FLAG_SHORTPRE;

	/* Set up the HT phy flags */
	if (rate_n_flags & RATE_MCS_HT_MSK)
		rx_status.flag |= RX_FLAG_HT;
	if (rate_n_flags & RATE_MCS_HT40_MSK)
		rx_status.flag |= RX_FLAG_40MHZ;
	if (rate_n_flags & RATE_MCS_SGI_MSK)
		rx_status.flag |= RX_FLAG_SHORT_GI;

	il4965_pass_packet_to_mac80211(il, header, len, ampdu_status,
				    rxb, &rx_status);
}

/* Cache phy data (Rx signal strength, etc) for HT frame (REPLY_RX_PHY_CMD).
 * This will be used later in il_rx_reply_rx() for REPLY_RX_MPDU_CMD. */
void il4965_rx_reply_rx_phy(struct il_priv *il,
			    struct il_rx_buf *rxb)
{
	struct il_rx_pkt *pkt = rxb_addr(rxb);
	il->_4965.last_phy_res_valid = true;
	memcpy(&il->_4965.last_phy_res, pkt->u.raw,
	       sizeof(struct il_rx_phy_res));
}

static int il4965_get_channels_for_scan(struct il_priv *il,
				     struct ieee80211_vif *vif,
				     enum ieee80211_band band,
				     u8 is_active, u8 n_probes,
				     struct il_scan_channel *scan_ch)
{
	struct ieee80211_channel *chan;
	const struct ieee80211_supported_band *sband;
	const struct il_channel_info *ch_info;
	u16 passive_dwell = 0;
	u16 active_dwell = 0;
	int added, i;
	u16 channel;

	sband = il_get_hw_mode(il, band);
	if (!sband)
		return 0;

	active_dwell = il_get_active_dwell_time(il, band, n_probes);
	passive_dwell = il_get_passive_dwell_time(il, band, vif);

	if (passive_dwell <= active_dwell)
		passive_dwell = active_dwell + 1;

	for (i = 0, added = 0; i < il->scan_request->n_channels; i++) {
		chan = il->scan_request->channels[i];

		if (chan->band != band)
			continue;

		channel = chan->hw_value;
		scan_ch->channel = cpu_to_le16(channel);

		ch_info = il_get_channel_info(il, band, channel);
		if (!il_is_channel_valid(ch_info)) {
			D_SCAN(
				 "Channel %d is INVALID for this band.\n",
					channel);
			continue;
		}

		if (!is_active || il_is_channel_passive(ch_info) ||
		    (chan->flags & IEEE80211_CHAN_PASSIVE_SCAN))
			scan_ch->type = SCAN_CHANNEL_TYPE_PASSIVE;
		else
			scan_ch->type = SCAN_CHANNEL_TYPE_ACTIVE;

		if (n_probes)
			scan_ch->type |= IL_SCAN_PROBE_MASK(n_probes);

		scan_ch->active_dwell = cpu_to_le16(active_dwell);
		scan_ch->passive_dwell = cpu_to_le16(passive_dwell);

		/* Set txpower levels to defaults */
		scan_ch->dsp_atten = 110;

		/* NOTE: if we were doing 6Mb OFDM for scans we'd use
		 * power level:
		 * scan_ch->tx_gain = ((1 << 5) | (2 << 3)) | 3;
		 */
		if (band == IEEE80211_BAND_5GHZ)
			scan_ch->tx_gain = ((1 << 5) | (3 << 3)) | 3;
		else
			scan_ch->tx_gain = ((1 << 5) | (5 << 3));

		D_SCAN("Scanning ch=%d prob=0x%X [%s %d]\n",
			       channel, le32_to_cpu(scan_ch->type),
			       (scan_ch->type & SCAN_CHANNEL_TYPE_ACTIVE) ?
				"ACTIVE" : "PASSIVE",
			       (scan_ch->type & SCAN_CHANNEL_TYPE_ACTIVE) ?
			       active_dwell : passive_dwell);

		scan_ch++;
		added++;
	}

	D_SCAN("total channels to scan %d\n", added);
	return added;
}

int il4965_request_scan(struct il_priv *il, struct ieee80211_vif *vif)
{
	struct il_host_cmd cmd = {
		.id = REPLY_SCAN_CMD,
		.len = sizeof(struct il_scan_cmd),
		.flags = CMD_SIZE_HUGE,
	};
	struct il_scan_cmd *scan;
	struct il_rxon_context *ctx = &il->ctx;
	u32 rate_flags = 0;
	u16 cmd_len;
	u16 rx_chain = 0;
	enum ieee80211_band band;
	u8 n_probes = 0;
	u8 rx_ant = il->hw_params.valid_rx_ant;
	u8 rate;
	bool is_active = false;
	int  chan_mod;
	u8 active_chains;
	u8 scan_tx_antennas = il->hw_params.valid_tx_ant;
	int ret;

	lockdep_assert_held(&il->mutex);

	if (vif)
		ctx = il_rxon_ctx_from_vif(vif);

	if (!il->scan_cmd) {
		il->scan_cmd = kmalloc(sizeof(struct il_scan_cmd) +
					 IL_MAX_SCAN_SIZE, GFP_KERNEL);
		if (!il->scan_cmd) {
			D_SCAN(
				       "fail to allocate memory for scan\n");
			return -ENOMEM;
		}
	}
	scan = il->scan_cmd;
	memset(scan, 0, sizeof(struct il_scan_cmd) + IL_MAX_SCAN_SIZE);

	scan->quiet_plcp_th = IL_PLCP_QUIET_THRESH;
	scan->quiet_time = IL_ACTIVE_QUIET_TIME;

	if (il_is_any_associated(il)) {
		u16 interval;
		u32 extra;
		u32 suspend_time = 100;
		u32 scan_suspend_time = 100;

		D_INFO("Scanning while associated...\n");
		interval = vif->bss_conf.beacon_int;

		scan->suspend_time = 0;
		scan->max_out_time = cpu_to_le32(200 * 1024);
		if (!interval)
			interval = suspend_time;

		extra = (suspend_time / interval) << 22;
		scan_suspend_time = (extra |
		    ((suspend_time % interval) * 1024));
		scan->suspend_time = cpu_to_le32(scan_suspend_time);
		D_SCAN("suspend_time 0x%X beacon interval %d\n",
			       scan_suspend_time, interval);
	}

	if (il->scan_request->n_ssids) {
		int i, p = 0;
		D_SCAN("Kicking off active scan\n");
		for (i = 0; i < il->scan_request->n_ssids; i++) {
			/* always does wildcard anyway */
			if (!il->scan_request->ssids[i].ssid_len)
				continue;
			scan->direct_scan[p].id = WLAN_EID_SSID;
			scan->direct_scan[p].len =
				il->scan_request->ssids[i].ssid_len;
			memcpy(scan->direct_scan[p].ssid,
			       il->scan_request->ssids[i].ssid,
			       il->scan_request->ssids[i].ssid_len);
			n_probes++;
			p++;
		}
		is_active = true;
	} else
		D_SCAN("Start passive scan.\n");

	scan->tx_cmd.tx_flags = TX_CMD_FLG_SEQ_CTL_MSK;
	scan->tx_cmd.sta_id = ctx->bcast_sta_id;
	scan->tx_cmd.stop_time.life_time = TX_CMD_LIFE_TIME_INFINITE;

	switch (il->scan_band) {
	case IEEE80211_BAND_2GHZ:
		scan->flags = RXON_FLG_BAND_24G_MSK | RXON_FLG_AUTO_DETECT_MSK;
		chan_mod = le32_to_cpu(
			il->ctx.active.flags &
						RXON_FLG_CHANNEL_MODE_MSK)
				       >> RXON_FLG_CHANNEL_MODE_POS;
		if (chan_mod == CHANNEL_MODE_PURE_40) {
			rate = IL_RATE_6M_PLCP;
		} else {
			rate = IL_RATE_1M_PLCP;
			rate_flags = RATE_MCS_CCK_MSK;
		}
		break;
	case IEEE80211_BAND_5GHZ:
		rate = IL_RATE_6M_PLCP;
		break;
	default:
		IL_WARN("Invalid scan band\n");
		return -EIO;
	}

	/*
	 * If active scanning is requested but a certain channel is
	 * marked passive, we can do active scanning if we detect
	 * transmissions.
	 *
	 * There is an issue with some firmware versions that triggers
	 * a sysassert on a "good CRC threshold" of zero (== disabled),
	 * on a radar channel even though this means that we should NOT
	 * send probes.
	 *
	 * The "good CRC threshold" is the number of frames that we
	 * need to receive during our dwell time on a channel before
	 * sending out probes -- setting this to a huge value will
	 * mean we never reach it, but at the same time work around
	 * the aforementioned issue. Thus use IL_GOOD_CRC_TH_NEVER
	 * here instead of IL_GOOD_CRC_TH_DISABLED.
	 */
	scan->good_CRC_th = is_active ? IL_GOOD_CRC_TH_DEFAULT :
					IL_GOOD_CRC_TH_NEVER;

	band = il->scan_band;

	if (il->cfg->scan_rx_antennas[band])
		rx_ant = il->cfg->scan_rx_antennas[band];

	il->scan_tx_ant[band] = il4965_toggle_tx_ant(il,
						il->scan_tx_ant[band],
						    scan_tx_antennas);
	rate_flags |= il4965_ant_idx_to_flags(il->scan_tx_ant[band]);
	scan->tx_cmd.rate_n_flags = il4965_hw_set_rate_n_flags(rate, rate_flags);

	/* In power save mode use one chain, otherwise use all chains */
	if (test_bit(STATUS_POWER_PMI, &il->status)) {
		/* rx_ant has been set to all valid chains previously */
		active_chains = rx_ant &
				((u8)(il->chain_noise_data.active_chains));
		if (!active_chains)
			active_chains = rx_ant;

		D_SCAN("chain_noise_data.active_chains: %u\n",
				il->chain_noise_data.active_chains);

		rx_ant = il4965_first_antenna(active_chains);
	}

	/* MIMO is not used here, but value is required */
	rx_chain |= il->hw_params.valid_rx_ant << RXON_RX_CHAIN_VALID_POS;
	rx_chain |= rx_ant << RXON_RX_CHAIN_FORCE_MIMO_SEL_POS;
	rx_chain |= rx_ant << RXON_RX_CHAIN_FORCE_SEL_POS;
	rx_chain |= 0x1 << RXON_RX_CHAIN_DRIVER_FORCE_POS;
	scan->rx_chain = cpu_to_le16(rx_chain);

	cmd_len = il_fill_probe_req(il,
					(struct ieee80211_mgmt *)scan->data,
					vif->addr,
					il->scan_request->ie,
					il->scan_request->ie_len,
					IL_MAX_SCAN_SIZE - sizeof(*scan));
	scan->tx_cmd.len = cpu_to_le16(cmd_len);

	scan->filter_flags |= (RXON_FILTER_ACCEPT_GRP_MSK |
			       RXON_FILTER_BCON_AWARE_MSK);

	scan->channel_count = il4965_get_channels_for_scan(il, vif, band,
						is_active, n_probes,
						(void *)&scan->data[cmd_len]);
	if (scan->channel_count == 0) {
		D_SCAN("channel count %d\n", scan->channel_count);
		return -EIO;
	}

	cmd.len += le16_to_cpu(scan->tx_cmd.len) +
	    scan->channel_count * sizeof(struct il_scan_channel);
	cmd.data = scan;
	scan->len = cpu_to_le16(cmd.len);

	set_bit(STATUS_SCAN_HW, &il->status);

	ret = il_send_cmd_sync(il, &cmd);
	if (ret)
		clear_bit(STATUS_SCAN_HW, &il->status);

	return ret;
}

int il4965_manage_ibss_station(struct il_priv *il,
			       struct ieee80211_vif *vif, bool add)
{
	struct il_vif_priv *vif_priv = (void *)vif->drv_priv;

	if (add)
		return il4965_add_bssid_station(il, vif_priv->ctx,
						vif->bss_conf.bssid,
						&vif_priv->ibss_bssid_sta_id);
	return il_remove_station(il, vif_priv->ibss_bssid_sta_id,
				  vif->bss_conf.bssid);
}

void il4965_free_tfds_in_queue(struct il_priv *il,
			    int sta_id, int tid, int freed)
{
	lockdep_assert_held(&il->sta_lock);

	if (il->stations[sta_id].tid[tid].tfds_in_queue >= freed)
		il->stations[sta_id].tid[tid].tfds_in_queue -= freed;
	else {
		D_TX("free more than tfds_in_queue (%u:%d)\n",
			il->stations[sta_id].tid[tid].tfds_in_queue,
			freed);
		il->stations[sta_id].tid[tid].tfds_in_queue = 0;
	}
}

#define IL_TX_QUEUE_MSK	0xfffff

static bool il4965_is_single_rx_stream(struct il_priv *il)
{
	return il->current_ht_config.smps == IEEE80211_SMPS_STATIC ||
	       il->current_ht_config.single_chain_sufficient;
}

#define IL_NUM_RX_CHAINS_MULTIPLE	3
#define IL_NUM_RX_CHAINS_SINGLE	2
#define IL_NUM_IDLE_CHAINS_DUAL	2
#define IL_NUM_IDLE_CHAINS_SINGLE	1

/*
 * Determine how many receiver/antenna chains to use.
 *
 * More provides better reception via diversity.  Fewer saves power
 * at the expense of throughput, but only when not in powersave to
 * start with.
 *
 * MIMO (dual stream) requires at least 2, but works better with 3.
 * This does not determine *which* chains to use, just how many.
 */
static int il4965_get_active_rx_chain_count(struct il_priv *il)
{
	/* # of Rx chains to use when expecting MIMO. */
	if (il4965_is_single_rx_stream(il))
		return IL_NUM_RX_CHAINS_SINGLE;
	else
		return IL_NUM_RX_CHAINS_MULTIPLE;
}

/*
 * When we are in power saving mode, unless device support spatial
 * multiplexing power save, use the active count for rx chain count.
 */
static int
il4965_get_idle_rx_chain_count(struct il_priv *il, int active_cnt)
{
	/* # Rx chains when idling, depending on SMPS mode */
	switch (il->current_ht_config.smps) {
	case IEEE80211_SMPS_STATIC:
	case IEEE80211_SMPS_DYNAMIC:
		return IL_NUM_IDLE_CHAINS_SINGLE;
	case IEEE80211_SMPS_OFF:
		return active_cnt;
	default:
		WARN(1, "invalid SMPS mode %d",
		     il->current_ht_config.smps);
		return active_cnt;
	}
}

/* up to 4 chains */
static u8 il4965_count_chain_bitmap(u32 chain_bitmap)
{
	u8 res;
	res = (chain_bitmap & BIT(0)) >> 0;
	res += (chain_bitmap & BIT(1)) >> 1;
	res += (chain_bitmap & BIT(2)) >> 2;
	res += (chain_bitmap & BIT(3)) >> 3;
	return res;
}

/**
 * il4965_set_rxon_chain - Set up Rx chain usage in "staging" RXON image
 *
 * Selects how many and which Rx receivers/antennas/chains to use.
 * This should not be used for scan command ... it puts data in wrong place.
 */
void il4965_set_rxon_chain(struct il_priv *il, struct il_rxon_context *ctx)
{
	bool is_single = il4965_is_single_rx_stream(il);
	bool is_cam = !test_bit(STATUS_POWER_PMI, &il->status);
	u8 idle_rx_cnt, active_rx_cnt, valid_rx_cnt;
	u32 active_chains;
	u16 rx_chain;

	/* Tell uCode which antennas are actually connected.
	 * Before first association, we assume all antennas are connected.
	 * Just after first association, il4965_chain_noise_calibration()
	 *    checks which antennas actually *are* connected. */
	if (il->chain_noise_data.active_chains)
		active_chains = il->chain_noise_data.active_chains;
	else
		active_chains = il->hw_params.valid_rx_ant;

	rx_chain = active_chains << RXON_RX_CHAIN_VALID_POS;

	/* How many receivers should we use? */
	active_rx_cnt = il4965_get_active_rx_chain_count(il);
	idle_rx_cnt = il4965_get_idle_rx_chain_count(il, active_rx_cnt);


	/* correct rx chain count according hw settings
	 * and chain noise calibration
	 */
	valid_rx_cnt = il4965_count_chain_bitmap(active_chains);
	if (valid_rx_cnt < active_rx_cnt)
		active_rx_cnt = valid_rx_cnt;

	if (valid_rx_cnt < idle_rx_cnt)
		idle_rx_cnt = valid_rx_cnt;

	rx_chain |= active_rx_cnt << RXON_RX_CHAIN_MIMO_CNT_POS;
	rx_chain |= idle_rx_cnt  << RXON_RX_CHAIN_CNT_POS;

	ctx->staging.rx_chain = cpu_to_le16(rx_chain);

	if (!is_single && active_rx_cnt >= IL_NUM_RX_CHAINS_SINGLE && is_cam)
		ctx->staging.rx_chain |= RXON_RX_CHAIN_MIMO_FORCE_MSK;
	else
		ctx->staging.rx_chain &= ~RXON_RX_CHAIN_MIMO_FORCE_MSK;

	D_ASSOC("rx_chain=0x%X active=%d idle=%d\n",
			ctx->staging.rx_chain,
			active_rx_cnt, idle_rx_cnt);

	WARN_ON(active_rx_cnt == 0 || idle_rx_cnt == 0 ||
		active_rx_cnt < idle_rx_cnt);
}

u8 il4965_toggle_tx_ant(struct il_priv *il, u8 ant, u8 valid)
{
	int i;
	u8 ind = ant;

	for (i = 0; i < RATE_ANT_NUM - 1; i++) {
		ind = (ind + 1) < RATE_ANT_NUM ?  ind + 1 : 0;
		if (valid & BIT(ind))
			return ind;
	}
	return ant;
}

static const char *il4965_get_fh_string(int cmd)
{
	switch (cmd) {
	IL_CMD(FH_RSCSR_CHNL0_STTS_WPTR_REG);
	IL_CMD(FH_RSCSR_CHNL0_RBDCB_BASE_REG);
	IL_CMD(FH_RSCSR_CHNL0_WPTR);
	IL_CMD(FH_MEM_RCSR_CHNL0_CONFIG_REG);
	IL_CMD(FH_MEM_RSSR_SHARED_CTRL_REG);
	IL_CMD(FH_MEM_RSSR_RX_STATUS_REG);
	IL_CMD(FH_MEM_RSSR_RX_ENABLE_ERR_IRQ2DRV);
	IL_CMD(FH_TSSR_TX_STATUS_REG);
	IL_CMD(FH_TSSR_TX_ERROR_REG);
	default:
		return "UNKNOWN";
	}
}

int il4965_dump_fh(struct il_priv *il, char **buf, bool display)
{
	int i;
#ifdef CONFIG_IWLEGACY_DEBUG
	int pos = 0;
	size_t bufsz = 0;
#endif
	static const u32 fh_tbl[] = {
		FH_RSCSR_CHNL0_STTS_WPTR_REG,
		FH_RSCSR_CHNL0_RBDCB_BASE_REG,
		FH_RSCSR_CHNL0_WPTR,
		FH_MEM_RCSR_CHNL0_CONFIG_REG,
		FH_MEM_RSSR_SHARED_CTRL_REG,
		FH_MEM_RSSR_RX_STATUS_REG,
		FH_MEM_RSSR_RX_ENABLE_ERR_IRQ2DRV,
		FH_TSSR_TX_STATUS_REG,
		FH_TSSR_TX_ERROR_REG
	};
#ifdef CONFIG_IWLEGACY_DEBUG
	if (display) {
		bufsz = ARRAY_SIZE(fh_tbl) * 48 + 40;
		*buf = kmalloc(bufsz, GFP_KERNEL);
		if (!*buf)
			return -ENOMEM;
		pos += scnprintf(*buf + pos, bufsz - pos,
				"FH register values:\n");
		for (i = 0; i < ARRAY_SIZE(fh_tbl); i++) {
			pos += scnprintf(*buf + pos, bufsz - pos,
				"  %34s: 0X%08x\n",
				il4965_get_fh_string(fh_tbl[i]),
				il_rd(il, fh_tbl[i]));
		}
		return pos;
	}
#endif
	IL_ERR("FH register values:\n");
	for (i = 0; i <  ARRAY_SIZE(fh_tbl); i++) {
		IL_ERR("  %34s: 0X%08x\n",
			il4965_get_fh_string(fh_tbl[i]),
			il_rd(il, fh_tbl[i]));
	}
	return 0;
}
