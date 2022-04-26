// SPDX-License-Identifier: GPL-2.0
/*
 * MHI Endpoint bus stack
 *
 * Copyright (C) 2022 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/mhi_ep.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include "internal.h"

static DEFINE_IDA(mhi_ep_cntrl_ida);

static int mhi_ep_send_event(struct mhi_ep_cntrl *mhi_cntrl, u32 ring_idx,
			     struct mhi_ring_element *el, bool bei)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	union mhi_ep_ring_ctx *ctx;
	struct mhi_ep_ring *ring;
	int ret;

	mutex_lock(&mhi_cntrl->event_lock);
	ring = &mhi_cntrl->mhi_event[ring_idx].ring;
	ctx = (union mhi_ep_ring_ctx *)&mhi_cntrl->ev_ctx_cache[ring_idx];
	if (!ring->started) {
		ret = mhi_ep_ring_start(mhi_cntrl, ring, ctx);
		if (ret) {
			dev_err(dev, "Error starting event ring (%u)\n", ring_idx);
			goto err_unlock;
		}
	}

	/* Add element to the event ring */
	ret = mhi_ep_ring_add_element(ring, el);
	if (ret) {
		dev_err(dev, "Error adding element to event ring (%u)\n", ring_idx);
		goto err_unlock;
	}

	mutex_unlock(&mhi_cntrl->event_lock);

	/*
	 * Raise IRQ to host only if the BEI flag is not set in TRE. Host might
	 * set this flag for interrupt moderation as per MHI protocol.
	 */
	if (!bei)
		mhi_cntrl->raise_irq(mhi_cntrl, ring->irq_vector);

	return 0;

err_unlock:
	mutex_unlock(&mhi_cntrl->event_lock);

	return ret;
}

static int mhi_ep_send_completion_event(struct mhi_ep_cntrl *mhi_cntrl, struct mhi_ep_ring *ring,
					struct mhi_ring_element *tre, u32 len, enum mhi_ev_ccs code)
{
	struct mhi_ring_element event = {};

	event.ptr = cpu_to_le64(ring->rbase + ring->rd_offset * sizeof(*tre));
	event.dword[0] = MHI_TRE_EV_DWORD0(code, len);
	event.dword[1] = MHI_TRE_EV_DWORD1(ring->ch_id, MHI_PKT_TYPE_TX_EVENT);

	return mhi_ep_send_event(mhi_cntrl, ring->er_index, &event, MHI_TRE_DATA_GET_BEI(tre));
}

int mhi_ep_send_state_change_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_state state)
{
	struct mhi_ring_element event = {};

	event.dword[0] = MHI_SC_EV_DWORD0(state);
	event.dword[1] = MHI_SC_EV_DWORD1(MHI_PKT_TYPE_STATE_CHANGE_EVENT);

	return mhi_ep_send_event(mhi_cntrl, 0, &event, 0);
}

int mhi_ep_send_ee_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ee_type exec_env)
{
	struct mhi_ring_element event = {};

	event.dword[0] = MHI_EE_EV_DWORD0(exec_env);
	event.dword[1] = MHI_SC_EV_DWORD1(MHI_PKT_TYPE_EE_EVENT);

	return mhi_ep_send_event(mhi_cntrl, 0, &event, 0);
}

static int mhi_ep_send_cmd_comp_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ev_ccs code)
{
	struct mhi_ep_ring *ring = &mhi_cntrl->mhi_cmd->ring;
	struct mhi_ring_element event = {};

	event.ptr = cpu_to_le64(ring->rbase + ring->rd_offset * sizeof(struct mhi_ring_element));
	event.dword[0] = MHI_CC_EV_DWORD0(code);
	event.dword[1] = MHI_CC_EV_DWORD1(MHI_PKT_TYPE_CMD_COMPLETION_EVENT);

	return mhi_ep_send_event(mhi_cntrl, 0, &event, 0);
}

static void mhi_ep_state_worker(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work, struct mhi_ep_cntrl, state_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_state_transition *itr, *tmp;
	unsigned long flags;
	LIST_HEAD(head);
	int ret;

	spin_lock_irqsave(&mhi_cntrl->list_lock, flags);
	list_splice_tail_init(&mhi_cntrl->st_transition_list, &head);
	spin_unlock_irqrestore(&mhi_cntrl->list_lock, flags);

	list_for_each_entry_safe(itr, tmp, &head, node) {
		list_del(&itr->node);
		dev_dbg(dev, "Handling MHI state transition to %s\n",
			 mhi_state_str(itr->state));

		switch (itr->state) {
		case MHI_STATE_M0:
			ret = mhi_ep_set_m0_state(mhi_cntrl);
			if (ret)
				dev_err(dev, "Failed to transition to M0 state\n");
			break;
		case MHI_STATE_M3:
			ret = mhi_ep_set_m3_state(mhi_cntrl);
			if (ret)
				dev_err(dev, "Failed to transition to M3 state\n");
			break;
		default:
			dev_err(dev, "Invalid MHI state transition: %d\n", itr->state);
			break;
		}
		kfree(itr);
	}
}

static void mhi_ep_queue_channel_db(struct mhi_ep_cntrl *mhi_cntrl, unsigned long ch_int,
				    u32 ch_idx)
{
	struct mhi_ep_ring_item *item;
	struct mhi_ep_ring *ring;
	bool work = !!ch_int;
	LIST_HEAD(head);
	u32 i;

	/* First add the ring items to a local list */
	for_each_set_bit(i, &ch_int, 32) {
		/* Channel index varies for each register: 0, 32, 64, 96 */
		u32 ch_id = ch_idx + i;

		ring = &mhi_cntrl->mhi_chan[ch_id].ring;
		item = kzalloc(sizeof(*item), GFP_ATOMIC);
		if (!item)
			return;

		item->ring = ring;
		list_add_tail(&item->node, &head);
	}

	/* Now, splice the local list into ch_db_list and queue the work item */
	if (work) {
		spin_lock(&mhi_cntrl->list_lock);
		list_splice_tail_init(&head, &mhi_cntrl->ch_db_list);
		spin_unlock(&mhi_cntrl->list_lock);
	}
}

/*
 * Channel interrupt statuses are contained in 4 registers each of 32bit length.
 * For checking all interrupts, we need to loop through each registers and then
 * check for bits set.
 */
static void mhi_ep_check_channel_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 ch_int, ch_idx, i;

	/* Bail out if there is no channel doorbell interrupt */
	if (!mhi_ep_mmio_read_chdb_status_interrupts(mhi_cntrl))
		return;

	for (i = 0; i < MHI_MASK_ROWS_CH_DB; i++) {
		ch_idx = i * MHI_MASK_CH_LEN;

		/* Only process channel interrupt if the mask is enabled */
		ch_int = mhi_cntrl->chdb[i].status & mhi_cntrl->chdb[i].mask;
		if (ch_int) {
			mhi_ep_queue_channel_db(mhi_cntrl, ch_int, ch_idx);
			mhi_ep_mmio_write(mhi_cntrl, MHI_CHDB_INT_CLEAR_n(i),
							mhi_cntrl->chdb[i].status);
		}
	}
}

static void mhi_ep_process_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl,
					 enum mhi_state state)
{
	struct mhi_ep_state_transition *item;

	item = kzalloc(sizeof(*item), GFP_ATOMIC);
	if (!item)
		return;

	item->state = state;
	spin_lock(&mhi_cntrl->list_lock);
	list_add_tail(&item->node, &mhi_cntrl->st_transition_list);
	spin_unlock(&mhi_cntrl->list_lock);

	queue_work(mhi_cntrl->wq, &mhi_cntrl->state_work);
}

/*
 * Interrupt handler that services interrupts raised by the host writing to
 * MHICTRL and Command ring doorbell (CRDB) registers for state change and
 * channel interrupts.
 */
static irqreturn_t mhi_ep_irq(int irq, void *data)
{
	struct mhi_ep_cntrl *mhi_cntrl = data;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	u32 int_value;

	/* Acknowledge the ctrl interrupt */
	int_value = mhi_ep_mmio_read(mhi_cntrl, MHI_CTRL_INT_STATUS);
	mhi_ep_mmio_write(mhi_cntrl, MHI_CTRL_INT_CLEAR, int_value);

	/* Check for ctrl interrupt */
	if (FIELD_GET(MHI_CTRL_INT_STATUS_MSK, int_value)) {
		dev_dbg(dev, "Processing ctrl interrupt\n");
		mhi_ep_process_ctrl_interrupt(mhi_cntrl, state);
	}

	/* Check for command doorbell interrupt */
	if (FIELD_GET(MHI_CTRL_INT_STATUS_CRDB_MSK, int_value))
		dev_dbg(dev, "Processing command doorbell interrupt\n");

	/* Check for channel interrupts */
	mhi_ep_check_channel_interrupt(mhi_cntrl);

	return IRQ_HANDLED;
}

static void mhi_ep_release_device(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);

	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		mhi_dev->mhi_cntrl->mhi_dev = NULL;

	/*
	 * We need to set the mhi_chan->mhi_dev to NULL here since the MHI
	 * devices for the channels will only get created in mhi_ep_create_device()
	 * if the mhi_dev associated with it is NULL.
	 */
	if (mhi_dev->ul_chan)
		mhi_dev->ul_chan->mhi_dev = NULL;

	if (mhi_dev->dl_chan)
		mhi_dev->dl_chan->mhi_dev = NULL;

	kfree(mhi_dev);
}

static struct mhi_ep_device *mhi_ep_alloc_device(struct mhi_ep_cntrl *mhi_cntrl,
						 enum mhi_device_type dev_type)
{
	struct mhi_ep_device *mhi_dev;
	struct device *dev;

	mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	if (!mhi_dev)
		return ERR_PTR(-ENOMEM);

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_ep_bus_type;
	dev->release = mhi_ep_release_device;

	/* Controller device is always allocated first */
	if (dev_type == MHI_DEVICE_CONTROLLER)
		/* for MHI controller device, parent is the bus device (e.g. PCI EPF) */
		dev->parent = mhi_cntrl->cntrl_dev;
	else
		/* for MHI client devices, parent is the MHI controller device */
		dev->parent = &mhi_cntrl->mhi_dev->dev;

	mhi_dev->mhi_cntrl = mhi_cntrl;
	mhi_dev->dev_type = dev_type;

	return mhi_dev;
}

/*
 * MHI channels are always defined in pairs with UL as the even numbered
 * channel and DL as odd numbered one. This function gets UL channel (primary)
 * as the ch_id and always looks after the next entry in channel list for
 * the corresponding DL channel (secondary).
 */
static int mhi_ep_create_device(struct mhi_ep_cntrl *mhi_cntrl, u32 ch_id)
{
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ch_id];
	struct device *dev = mhi_cntrl->cntrl_dev;
	struct mhi_ep_device *mhi_dev;
	int ret;

	/* Check if the channel name is same for both UL and DL */
	if (strcmp(mhi_chan->name, mhi_chan[1].name)) {
		dev_err(dev, "UL and DL channel names are not same: (%s) != (%s)\n",
			mhi_chan->name, mhi_chan[1].name);
		return -EINVAL;
	}

	mhi_dev = mhi_ep_alloc_device(mhi_cntrl, MHI_DEVICE_XFER);
	if (IS_ERR(mhi_dev))
		return PTR_ERR(mhi_dev);

	/* Configure primary channel */
	mhi_dev->ul_chan = mhi_chan;
	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Configure secondary channel as well */
	mhi_chan++;
	mhi_dev->dl_chan = mhi_chan;
	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Channel name is same for both UL and DL */
	mhi_dev->name = mhi_chan->name;
	dev_set_name(&mhi_dev->dev, "%s_%s",
		     dev_name(&mhi_cntrl->mhi_dev->dev),
		     mhi_dev->name);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		put_device(&mhi_dev->dev);

	return ret;
}

static int mhi_ep_destroy_device(struct device *dev, void *data)
{
	struct mhi_ep_device *mhi_dev;
	struct mhi_ep_cntrl *mhi_cntrl;
	struct mhi_ep_chan *ul_chan, *dl_chan;

	if (dev->bus != &mhi_ep_bus_type)
		return 0;

	mhi_dev = to_mhi_ep_device(dev);
	mhi_cntrl = mhi_dev->mhi_cntrl;

	/* Only destroy devices created for channels */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	ul_chan = mhi_dev->ul_chan;
	dl_chan = mhi_dev->dl_chan;

	if (ul_chan)
		put_device(&ul_chan->mhi_dev->dev);

	if (dl_chan)
		put_device(&dl_chan->mhi_dev->dev);

	dev_dbg(&mhi_cntrl->mhi_dev->dev, "Destroying device for chan:%s\n",
		 mhi_dev->name);

	/* Notify the client and remove the device from MHI bus */
	device_del(dev);
	put_device(dev);

	return 0;
}

static int mhi_ep_chan_init(struct mhi_ep_cntrl *mhi_cntrl,
			    const struct mhi_ep_cntrl_config *config)
{
	const struct mhi_ep_channel_config *ch_cfg;
	struct device *dev = mhi_cntrl->cntrl_dev;
	u32 chan, i;
	int ret = -EINVAL;

	mhi_cntrl->max_chan = config->max_channels;

	/*
	 * Allocate max_channels supported by the MHI endpoint and populate
	 * only the defined channels
	 */
	mhi_cntrl->mhi_chan = kcalloc(mhi_cntrl->max_chan, sizeof(*mhi_cntrl->mhi_chan),
				      GFP_KERNEL);
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;

	for (i = 0; i < config->num_channels; i++) {
		struct mhi_ep_chan *mhi_chan;

		ch_cfg = &config->ch_cfg[i];

		chan = ch_cfg->num;
		if (chan >= mhi_cntrl->max_chan) {
			dev_err(dev, "Channel (%u) exceeds maximum available channels (%u)\n",
				chan, mhi_cntrl->max_chan);
			goto error_chan_cfg;
		}

		/* Bi-directional and direction less channels are not supported */
		if (ch_cfg->dir == DMA_BIDIRECTIONAL || ch_cfg->dir == DMA_NONE) {
			dev_err(dev, "Invalid direction (%u) for channel (%u)\n",
				ch_cfg->dir, chan);
			goto error_chan_cfg;
		}

		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		mhi_chan->name = ch_cfg->name;
		mhi_chan->chan = chan;
		mhi_chan->dir = ch_cfg->dir;
		mutex_init(&mhi_chan->lock);
	}

	return 0;

error_chan_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}

/*
 * Allocate channel and command rings here. Event rings will be allocated
 * in mhi_ep_power_up() as the config comes from the host.
 */
int mhi_ep_register_controller(struct mhi_ep_cntrl *mhi_cntrl,
				const struct mhi_ep_cntrl_config *config)
{
	struct mhi_ep_device *mhi_dev;
	int ret;

	if (!mhi_cntrl || !mhi_cntrl->cntrl_dev || !mhi_cntrl->mmio || !mhi_cntrl->irq)
		return -EINVAL;

	ret = mhi_ep_chan_init(mhi_cntrl, config);
	if (ret)
		return ret;

	mhi_cntrl->mhi_cmd = kcalloc(NR_OF_CMD_RINGS, sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd) {
		ret = -ENOMEM;
		goto err_free_ch;
	}

	INIT_WORK(&mhi_cntrl->state_work, mhi_ep_state_worker);

	mhi_cntrl->wq = alloc_workqueue("mhi_ep_wq", 0, 0);
	if (!mhi_cntrl->wq) {
		ret = -ENOMEM;
		goto err_free_cmd;
	}

	INIT_LIST_HEAD(&mhi_cntrl->st_transition_list);
	INIT_LIST_HEAD(&mhi_cntrl->ch_db_list);
	spin_lock_init(&mhi_cntrl->state_lock);
	spin_lock_init(&mhi_cntrl->list_lock);
	mutex_init(&mhi_cntrl->event_lock);

	/* Set MHI version and AMSS EE before enumeration */
	mhi_ep_mmio_write(mhi_cntrl, EP_MHIVER, config->mhi_version);
	mhi_ep_mmio_set_env(mhi_cntrl, MHI_EE_AMSS);

	/* Set controller index */
	ret = ida_alloc(&mhi_ep_cntrl_ida, GFP_KERNEL);
	if (ret < 0)
		goto err_destroy_wq;

	mhi_cntrl->index = ret;

	irq_set_status_flags(mhi_cntrl->irq, IRQ_NOAUTOEN);
	ret = request_irq(mhi_cntrl->irq, mhi_ep_irq, IRQF_TRIGGER_HIGH,
			  "doorbell_irq", mhi_cntrl);
	if (ret) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to request Doorbell IRQ\n");
		goto err_ida_free;
	}

	/* Allocate the controller device */
	mhi_dev = mhi_ep_alloc_device(mhi_cntrl, MHI_DEVICE_CONTROLLER);
	if (IS_ERR(mhi_dev)) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to allocate controller device\n");
		ret = PTR_ERR(mhi_dev);
		goto err_free_irq;
	}

	dev_set_name(&mhi_dev->dev, "mhi_ep%u", mhi_cntrl->index);
	mhi_dev->name = dev_name(&mhi_dev->dev);
	mhi_cntrl->mhi_dev = mhi_dev;

	ret = device_add(&mhi_dev->dev);
	if (ret)
		goto err_put_dev;

	dev_dbg(&mhi_dev->dev, "MHI EP Controller registered\n");

	return 0;

err_put_dev:
	put_device(&mhi_dev->dev);
err_free_irq:
	free_irq(mhi_cntrl->irq, mhi_cntrl);
err_ida_free:
	ida_free(&mhi_ep_cntrl_ida, mhi_cntrl->index);
err_destroy_wq:
	destroy_workqueue(mhi_cntrl->wq);
err_free_cmd:
	kfree(mhi_cntrl->mhi_cmd);
err_free_ch:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_ep_register_controller);

void mhi_ep_unregister_controller(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_device *mhi_dev = mhi_cntrl->mhi_dev;

	destroy_workqueue(mhi_cntrl->wq);

	free_irq(mhi_cntrl->irq, mhi_cntrl);

	kfree(mhi_cntrl->mhi_cmd);
	kfree(mhi_cntrl->mhi_chan);

	device_del(&mhi_dev->dev);
	put_device(&mhi_dev->dev);

	ida_free(&mhi_ep_cntrl_ida, mhi_cntrl->index);
}
EXPORT_SYMBOL_GPL(mhi_ep_unregister_controller);

static int mhi_ep_driver_probe(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(dev->driver);
	struct mhi_ep_chan *ul_chan = mhi_dev->ul_chan;
	struct mhi_ep_chan *dl_chan = mhi_dev->dl_chan;

	ul_chan->xfer_cb = mhi_drv->ul_xfer_cb;
	dl_chan->xfer_cb = mhi_drv->dl_xfer_cb;

	return mhi_drv->probe(mhi_dev, mhi_dev->id);
}

static int mhi_ep_driver_remove(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(dev->driver);
	struct mhi_result result = {};
	struct mhi_ep_chan *mhi_chan;
	int dir;

	/* Skip if it is a controller device */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	/* Disconnect the channels associated with the driver */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		mutex_lock(&mhi_chan->lock);
		/* Send channel disconnect status to the client driver */
		if (mhi_chan->xfer_cb) {
			result.transaction_status = -ENOTCONN;
			result.bytes_xferd = 0;
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
		}

		mhi_chan->state = MHI_CH_STATE_DISABLED;
		mhi_chan->xfer_cb = NULL;
		mutex_unlock(&mhi_chan->lock);
	}

	/* Remove the client driver now */
	mhi_drv->remove(mhi_dev);

	return 0;
}

int __mhi_ep_driver_register(struct mhi_ep_driver *mhi_drv, struct module *owner)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	/* Client drivers should have callbacks defined for both channels */
	if (!mhi_drv->ul_xfer_cb || !mhi_drv->dl_xfer_cb)
		return -EINVAL;

	driver->bus = &mhi_ep_bus_type;
	driver->owner = owner;
	driver->probe = mhi_ep_driver_probe;
	driver->remove = mhi_ep_driver_remove;

	return driver_register(driver);
}
EXPORT_SYMBOL_GPL(__mhi_ep_driver_register);

void mhi_ep_driver_unregister(struct mhi_ep_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL_GPL(mhi_ep_driver_unregister);

static int mhi_ep_match(struct device *dev, struct device_driver *drv)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(drv);
	const struct mhi_device_id *id;

	/*
	 * If the device is a controller type then there is no client driver
	 * associated with it
	 */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	for (id = mhi_drv->id_table; id->chan[0]; id++)
		if (!strcmp(mhi_dev->name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

struct bus_type mhi_ep_bus_type = {
	.name = "mhi_ep",
	.dev_name = "mhi_ep",
	.match = mhi_ep_match,
};

static int __init mhi_ep_init(void)
{
	return bus_register(&mhi_ep_bus_type);
}

static void __exit mhi_ep_exit(void)
{
	bus_unregister(&mhi_ep_bus_type);
}

postcore_initcall(mhi_ep_init);
module_exit(mhi_ep_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI Bus Endpoint stack");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
