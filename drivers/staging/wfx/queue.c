// SPDX-License-Identifier: GPL-2.0-only
/*
 * O(1) TX queue with built-in allocator.
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#include <linux/sched.h>
#include <net/mac80211.h>

#include "queue.h"
#include "wfx.h"
#include "sta.h"
#include "data_tx.h"

void wfx_tx_lock(struct wfx_dev *wdev)
{
	atomic_inc(&wdev->tx_lock);
}

void wfx_tx_unlock(struct wfx_dev *wdev)
{
	int tx_lock = atomic_dec_return(&wdev->tx_lock);

	WARN(tx_lock < 0, "inconsistent tx_lock value");
	if (!tx_lock)
		wfx_bh_request_tx(wdev);
}

void wfx_tx_flush(struct wfx_dev *wdev)
{
	int ret;

	// Do not wait for any reply if chip is frozen
	if (wdev->chip_frozen)
		return;

	mutex_lock(&wdev->hif_cmd.lock);
	ret = wait_event_timeout(wdev->hif.tx_buffers_empty,
				 !wdev->hif.tx_buffers_used,
				 msecs_to_jiffies(3000));
	if (!ret) {
		dev_warn(wdev->dev, "cannot flush tx buffers (%d still busy)\n",
			 wdev->hif.tx_buffers_used);
		wfx_pending_dump_old_frames(wdev, 3000);
		// FIXME: drop pending frames here
		wdev->chip_frozen = 1;
	}
	mutex_unlock(&wdev->hif_cmd.lock);
}

void wfx_tx_lock_flush(struct wfx_dev *wdev)
{
	wfx_tx_lock(wdev);
	wfx_tx_flush(wdev);
}

/* If successful, LOCKS the TX queue! */
void wfx_tx_queues_wait_empty_vif(struct wfx_vif *wvif)
{
	int i;
	bool done;
	struct wfx_queue *queue;
	struct sk_buff *item;
	struct wfx_dev *wdev = wvif->wdev;
	struct hif_msg *hif;

	if (wvif->wdev->chip_frozen) {
		wfx_tx_lock_flush(wdev);
		wfx_tx_queues_clear(wdev);
		return;
	}

	do {
		done = true;
		wfx_tx_lock_flush(wdev);
		for (i = 0; i < IEEE80211_NUM_ACS && done; ++i) {
			queue = &wdev->tx_queue[i];
			spin_lock_bh(&queue->queue.lock);
			skb_queue_walk(&queue->queue, item) {
				hif = (struct hif_msg *) item->data;
				if (hif->interface == wvif->id)
					done = false;
			}
			spin_unlock_bh(&queue->queue.lock);
		}
		if (!done) {
			wfx_tx_unlock(wdev);
			msleep(20);
		}
	} while (!done);
}

static void wfx_tx_queue_clear(struct wfx_dev *wdev, struct wfx_queue *queue,
			       struct sk_buff_head *gc_list)
{
	struct sk_buff *item;

	while ((item = skb_dequeue(&queue->queue)) != NULL)
		skb_queue_head(gc_list, item);
}

void wfx_tx_queues_clear(struct wfx_dev *wdev)
{
	int i;
	struct sk_buff *item;
	struct sk_buff_head gc_list;
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;

	skb_queue_head_init(&gc_list);
	for (i = 0; i < IEEE80211_NUM_ACS; ++i)
		wfx_tx_queue_clear(wdev, &wdev->tx_queue[i], &gc_list);
	wake_up(&stats->wait_link_id_empty);
	while ((item = skb_dequeue(&gc_list)) != NULL)
		wfx_skb_dtor(wdev, item);
}

void wfx_tx_queues_init(struct wfx_dev *wdev)
{
	int i;

	memset(&wdev->tx_queue_stats, 0, sizeof(wdev->tx_queue_stats));
	memset(wdev->tx_queue, 0, sizeof(wdev->tx_queue));
	skb_queue_head_init(&wdev->tx_queue_stats.pending);
	init_waitqueue_head(&wdev->tx_queue_stats.wait_link_id_empty);

	for (i = 0; i < IEEE80211_NUM_ACS; ++i)
		skb_queue_head_init(&wdev->tx_queue[i].queue);
}

void wfx_tx_queues_deinit(struct wfx_dev *wdev)
{
	WARN_ON(!skb_queue_empty(&wdev->tx_queue_stats.pending));
	wfx_tx_queues_clear(wdev);
}

int wfx_tx_queue_get_num_queued(struct wfx_queue *queue)
{
	struct ieee80211_tx_info *tx_info;
	struct sk_buff *skb;
	int ret = 0;

	spin_lock_bh(&queue->queue.lock);
	skb_queue_walk(&queue->queue, skb) {
		tx_info = IEEE80211_SKB_CB(skb);
		if (!(tx_info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM))
			ret++;
	}
	spin_unlock_bh(&queue->queue.lock);
	return ret;
}

void wfx_tx_queue_put(struct wfx_dev *wdev, struct wfx_queue *queue,
		      struct sk_buff *skb)
{
	skb_queue_tail(&queue->queue, skb);
}

static struct sk_buff *wfx_tx_queue_get(struct wfx_dev *wdev,
					struct wfx_queue *queue,
					bool mcast)
{
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;
	struct ieee80211_tx_info *tx_info;
	struct sk_buff *item, *skb = NULL;
	struct wfx_tx_priv *tx_priv;

	spin_lock_bh(&queue->queue.lock);
	skb_queue_walk(&queue->queue, item) {
		tx_info = IEEE80211_SKB_CB(item);
		if (mcast == !!(tx_info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM)) {
			skb = item;
			break;
		}
	}
	spin_unlock_bh(&queue->queue.lock);
	if (skb) {
		skb_unlink(skb, &queue->queue);
		tx_priv = wfx_skb_tx_priv(skb);
		tx_priv->xmit_timestamp = ktime_get();
		skb_queue_tail(&stats->pending, skb);
		if (skb_queue_empty(&queue->queue))
			wake_up(&stats->wait_link_id_empty);
		return skb;
	}
	return skb;
}

int wfx_pending_requeue(struct wfx_dev *wdev, struct sk_buff *skb)
{
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;
	struct wfx_queue *queue = &wdev->tx_queue[skb_get_queue_mapping(skb)];

	WARN_ON(skb_get_queue_mapping(skb) > 3);

	skb_unlink(skb, &stats->pending);
	skb_queue_tail(&queue->queue, skb);
	return 0;
}

int wfx_pending_remove(struct wfx_dev *wdev, struct sk_buff *skb)
{
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;

	skb_unlink(skb, &stats->pending);
	wfx_skb_dtor(wdev, skb);

	return 0;
}

struct sk_buff *wfx_pending_get(struct wfx_dev *wdev, u32 packet_id)
{
	struct sk_buff *skb;
	struct hif_req_tx *req;
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;

	spin_lock_bh(&stats->pending.lock);
	skb_queue_walk(&stats->pending, skb) {
		req = wfx_skb_txreq(skb);
		if (req->packet_id == packet_id) {
			spin_unlock_bh(&stats->pending.lock);
			return skb;
		}
	}
	spin_unlock_bh(&stats->pending.lock);
	WARN(1, "cannot find packet in pending queue");
	return NULL;
}

void wfx_pending_dump_old_frames(struct wfx_dev *wdev, unsigned int limit_ms)
{
	struct wfx_queue_stats *stats = &wdev->tx_queue_stats;
	ktime_t now = ktime_get();
	struct wfx_tx_priv *tx_priv;
	struct hif_req_tx *req;
	struct sk_buff *skb;
	bool first = true;

	spin_lock_bh(&stats->pending.lock);
	skb_queue_walk(&stats->pending, skb) {
		tx_priv = wfx_skb_tx_priv(skb);
		req = wfx_skb_txreq(skb);
		if (ktime_after(now, ktime_add_ms(tx_priv->xmit_timestamp,
						  limit_ms))) {
			if (first) {
				dev_info(wdev->dev, "frames stuck in firmware since %dms or more:\n",
					 limit_ms);
				first = false;
			}
			dev_info(wdev->dev, "   id %08x sent %lldms ago\n",
				 req->packet_id,
				 ktime_ms_delta(now, tx_priv->xmit_timestamp));
		}
	}
	spin_unlock_bh(&stats->pending.lock);
}

unsigned int wfx_pending_get_pkt_us_delay(struct wfx_dev *wdev,
					  struct sk_buff *skb)
{
	ktime_t now = ktime_get();
	struct wfx_tx_priv *tx_priv = wfx_skb_tx_priv(skb);

	return ktime_us_delta(now, tx_priv->xmit_timestamp);
}

bool wfx_tx_queues_empty(struct wfx_dev *wdev)
{
	int i;

	for (i = 0; i < IEEE80211_NUM_ACS; i++)
		if (!skb_queue_empty_lockless(&wdev->tx_queue[i].queue))
			return false;
	return true;
}

static bool wfx_handle_tx_data(struct wfx_dev *wdev, struct sk_buff *skb)
{
	struct hif_req_tx *req = wfx_skb_txreq(skb);
	struct ieee80211_key_conf *hw_key = wfx_skb_tx_priv(skb)->hw_key;
	struct ieee80211_hdr *frame =
		(struct ieee80211_hdr *)(req->frame + req->data_flags.fc_offset);
	struct wfx_vif *wvif =
		wdev_to_wvif(wdev, ((struct hif_msg *)skb->data)->interface);

	if (!wvif)
		return false;

	// FIXME: mac80211 is smart enough to handle BSS loss. Driver should not
	// try to do anything about that.
	if (ieee80211_is_nullfunc(frame->frame_control)) {
		mutex_lock(&wvif->bss_loss_lock);
		if (wvif->bss_loss_state) {
			wvif->bss_loss_confirm_id = req->packet_id;
			req->queue_id.queue_id = HIF_QUEUE_ID_VOICE;
		}
		mutex_unlock(&wvif->bss_loss_lock);
	}

	// FIXME: identify the exact scenario matched by this condition. Does it
	// happen yet?
	if (ieee80211_has_protected(frame->frame_control) &&
	    hw_key && hw_key->keyidx != wvif->wep_default_key_id &&
	    (hw_key->cipher == WLAN_CIPHER_SUITE_WEP40 ||
	     hw_key->cipher == WLAN_CIPHER_SUITE_WEP104)) {
		wfx_tx_lock(wdev);
		WARN_ON(wvif->wep_pending_skb);
		wvif->wep_default_key_id = hw_key->keyidx;
		wvif->wep_pending_skb = skb;
		if (!schedule_work(&wvif->wep_key_work))
			wfx_tx_unlock(wdev);
		return true;
	} else {
		return false;
	}
}

static struct wfx_queue *wfx_tx_queue_mask_get(struct wfx_vif *wvif)
{
	const struct ieee80211_tx_queue_params *edca;
	unsigned int score, best = -1;
	int winner = -1;
	int i;

	/* search for a winner using edca params */
	for (i = 0; i < IEEE80211_NUM_ACS; ++i) {
		int queued;

		edca = &wvif->edca_params[i];
		queued = wfx_tx_queue_get_num_queued(&wvif->wdev->tx_queue[i]);
		if (!queued)
			continue;
		score = ((edca->aifs + edca->cw_min) << 16) +
			((edca->cw_max - edca->cw_min) *
			 (get_random_int() & 0xFFFF));
		if (score < best && (winner < 0 || i != 3)) {
			best = score;
			winner = i;
		}
	}

	if (winner < 0)
		return NULL;
	return &wvif->wdev->tx_queue[winner];
}

struct hif_msg *wfx_tx_queues_get_after_dtim(struct wfx_vif *wvif)
{
	struct wfx_dev *wdev = wvif->wdev;
	struct ieee80211_tx_info *tx_info;
	struct hif_msg *hif;
	struct sk_buff *skb;
	int i;

	for (i = 0; i < IEEE80211_NUM_ACS; ++i) {
		skb_queue_walk(&wdev->tx_queue[i].queue, skb) {
			tx_info = IEEE80211_SKB_CB(skb);
			hif = (struct hif_msg *)skb->data;
			if ((tx_info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM) &&
			    (hif->interface == wvif->id))
				return (struct hif_msg *)skb->data;
		}
	}
	return NULL;
}

struct hif_msg *wfx_tx_queues_get(struct wfx_dev *wdev)
{
	struct sk_buff *skb;
	struct hif_msg *hif = NULL;
	struct wfx_queue *queue = NULL;
	struct wfx_queue *vif_queue = NULL;
	struct wfx_vif *wvif;
	int i;

	if (atomic_read(&wdev->tx_lock))
		return NULL;

	wvif = NULL;
	while ((wvif = wvif_iterate(wdev, wvif)) != NULL) {
		if (wvif->after_dtim_tx_allowed) {
			for (i = 0; i < IEEE80211_NUM_ACS; ++i) {
				skb = wfx_tx_queue_get(wvif->wdev,
						       &wdev->tx_queue[i],
						       true);
				if (skb) {
					hif = (struct hif_msg *)skb->data;
					// Cannot happen since only one vif can
					// be AP at time
					WARN_ON(wvif->id != hif->interface);
					return hif;
				}
			}
			// No more multicast to sent
			wvif->after_dtim_tx_allowed = false;
			schedule_work(&wvif->update_tim_work);
		}
	}

	for (;;) {
		int ret = -ENOENT;
		int queue_num;

		wvif = NULL;
		while ((wvif = wvif_iterate(wdev, wvif)) != NULL) {
			vif_queue = wfx_tx_queue_mask_get(wvif);
			if (vif_queue) {
				if (queue && queue != vif_queue)
					dev_info(wdev->dev, "vifs disagree about queue priority\n");
				queue = vif_queue;
				ret = 0;
			}
		}

		if (ret)
			return NULL;

		queue_num = queue - wdev->tx_queue;

		skb = wfx_tx_queue_get(wdev, queue, false);
		if (!skb)
			continue;

		if (wfx_handle_tx_data(wdev, skb))
			continue;  /* Handled by WSM */

		return (struct hif_msg *)skb->data;
	}
}
