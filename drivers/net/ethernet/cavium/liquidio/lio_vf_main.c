/**********************************************************************
 * Author: Cavium, Inc.
 *
 * Contact: support@cavium.com
 *          Please include "LiquidIO" in the subject.
 *
 * Copyright (c) 2003-2016 Cavium, Inc.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, Version 2, as
 * published by the Free Software Foundation.
 *
 * This file is distributed in the hope that it will be useful, but
 * AS-IS and WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, TITLE, or
 * NONINFRINGEMENT.  See the GNU General Public License for more details.
 ***********************************************************************/
#include <linux/pci.h>
#include <net/vxlan.h>
#include "liquidio_common.h"
#include "octeon_droq.h"
#include "octeon_iq.h"
#include "response_manager.h"
#include "octeon_device.h"
#include "octeon_nic.h"
#include "octeon_main.h"
#include "octeon_network.h"
#include "cn23xx_vf_device.h"

MODULE_AUTHOR("Cavium Networks, <support@cavium.com>");
MODULE_DESCRIPTION("Cavium LiquidIO Intelligent Server Adapter Virtual Function Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(LIQUIDIO_VERSION);

static int debug = -1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "NETIF_MSG debug bits");

#define DEFAULT_MSG_ENABLE (NETIF_MSG_DRV | NETIF_MSG_PROBE | NETIF_MSG_LINK)

#define   LIO_IFSTATE_REGISTERED           0x02
#define   LIO_IFSTATE_RUNNING              0x04

struct liquidio_if_cfg_context {
	int octeon_id;

	wait_queue_head_t wc;

	int cond;
};

struct liquidio_if_cfg_resp {
	u64 rh;
	struct liquidio_if_cfg_info cfg_info;
	u64 status;
};

#define OCTNIC_GSO_MAX_HEADER_SIZE 128
#define OCTNIC_GSO_MAX_SIZE \
		(CN23XX_DEFAULT_INPUT_JABBER - OCTNIC_GSO_MAX_HEADER_SIZE)

struct octeon_device_priv {
	/* Tasklet structures for this device. */
	struct tasklet_struct droq_tasklet;
	unsigned long napi_mask;
};

static int
liquidio_vf_probe(struct pci_dev *pdev, const struct pci_device_id *ent);
static void liquidio_vf_remove(struct pci_dev *pdev);
static int octeon_device_init(struct octeon_device *oct);
static int liquidio_stop(struct net_device *netdev);

static int lio_wait_for_oq_pkts(struct octeon_device *oct)
{
	struct octeon_device_priv *oct_priv =
	    (struct octeon_device_priv *)oct->priv;
	int retry = MAX_VF_IP_OP_PENDING_PKT_COUNT;
	int pkt_cnt = 0, pending_pkts;
	int i;

	do {
		pending_pkts = 0;

		for (i = 0; i < MAX_OCTEON_OUTPUT_QUEUES(oct); i++) {
			if (!(oct->io_qmask.oq & BIT_ULL(i)))
				continue;
			pkt_cnt += octeon_droq_check_hw_for_pkts(oct->droq[i]);
		}
		if (pkt_cnt > 0) {
			pending_pkts += pkt_cnt;
			tasklet_schedule(&oct_priv->droq_tasklet);
		}
		pkt_cnt = 0;
		schedule_timeout_uninterruptible(1);

	} while (retry-- && pending_pkts);

	return pkt_cnt;
}

/**
 * \brief wait for all pending requests to complete
 * @param oct Pointer to Octeon device
 *
 * Called during shutdown sequence
 */
static int wait_for_pending_requests(struct octeon_device *oct)
{
	int i, pcount = 0;

	for (i = 0; i < MAX_VF_IP_OP_PENDING_PKT_COUNT; i++) {
		pcount = atomic_read(
		    &oct->response_list[OCTEON_ORDERED_SC_LIST]
			 .pending_req_count);
		if (pcount)
			schedule_timeout_uninterruptible(HZ / 10);
		else
			break;
	}

	if (pcount)
		return 1;

	return 0;
}

static const struct pci_device_id liquidio_vf_pci_tbl[] = {
	{
		PCI_VENDOR_ID_CAVIUM, OCTEON_CN23XX_VF_VID,
		PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0
	},
	{
		0, 0, 0, 0, 0, 0, 0
	}
};
MODULE_DEVICE_TABLE(pci, liquidio_vf_pci_tbl);

static struct pci_driver liquidio_vf_pci_driver = {
	.name		= "LiquidIO_VF",
	.id_table	= liquidio_vf_pci_tbl,
	.probe		= liquidio_vf_probe,
	.remove		= liquidio_vf_remove,
};

/**
 * \brief set interface state
 * @param lio per-network private data
 * @param state_flag flag state to set
 */
static void ifstate_set(struct lio *lio, int state_flag)
{
	atomic_set(&lio->ifstate, (atomic_read(&lio->ifstate) | state_flag));
}

/**
 * \brief clear interface state
 * @param lio per-network private data
 * @param state_flag flag state to clear
 */
static void ifstate_reset(struct lio *lio, int state_flag)
{
	atomic_set(&lio->ifstate, (atomic_read(&lio->ifstate) & ~(state_flag)));
}

static
int liquidio_schedule_msix_droq_pkt_handler(struct octeon_droq *droq, u64 ret)
{
	struct octeon_device *oct = droq->oct_dev;
	struct octeon_device_priv *oct_priv =
	    (struct octeon_device_priv *)oct->priv;

	if (droq->ops.poll_mode) {
		droq->ops.napi_fn(droq);
	} else {
		if (ret & MSIX_PO_INT) {
			dev_err(&oct->pci_dev->dev,
				"should not come here should not get rx when poll mode = 0 for vf\n");
			tasklet_schedule(&oct_priv->droq_tasklet);
			return 1;
		}
		/* this will be flushed periodically by check iq db */
		if (ret & MSIX_PI_INT)
			return 0;
	}
	return 0;
}

static irqreturn_t
liquidio_msix_intr_handler(int irq __attribute__((unused)), void *dev)
{
	struct octeon_ioq_vector *ioq_vector = (struct octeon_ioq_vector *)dev;
	struct octeon_device *oct = ioq_vector->oct_dev;
	struct octeon_droq *droq = oct->droq[ioq_vector->droq_index];
	u64 ret;

	ret = oct->fn_list.msix_interrupt_handler(ioq_vector);

	if ((ret & MSIX_PO_INT) || (ret & MSIX_PI_INT))
		liquidio_schedule_msix_droq_pkt_handler(droq, ret);

	return IRQ_HANDLED;
}

/**
 * \brief Setup interrupt for octeon device
 * @param oct octeon device
 *
 *  Enable interrupt in Octeon device as given in the PCI interrupt mask.
 */
static int octeon_setup_interrupt(struct octeon_device *oct)
{
	struct msix_entry *msix_entries;
	int num_alloc_ioq_vectors;
	int num_ioq_vectors;
	int irqret;
	int i;

	if (oct->msix_on) {
		oct->num_msix_irqs = oct->sriov_info.rings_per_vf;

		oct->msix_entries = kcalloc(
		    oct->num_msix_irqs, sizeof(struct msix_entry), GFP_KERNEL);
		if (!oct->msix_entries)
			return 1;

		msix_entries = (struct msix_entry *)oct->msix_entries;

		for (i = 0; i < oct->num_msix_irqs; i++)
			msix_entries[i].entry = i;
		num_alloc_ioq_vectors = pci_enable_msix_range(
						oct->pci_dev, msix_entries,
						oct->num_msix_irqs,
						oct->num_msix_irqs);
		if (num_alloc_ioq_vectors < 0) {
			dev_err(&oct->pci_dev->dev, "unable to Allocate MSI-X interrupts\n");
			kfree(oct->msix_entries);
			oct->msix_entries = NULL;
			return 1;
		}
		dev_dbg(&oct->pci_dev->dev, "OCTEON: Enough MSI-X interrupts are allocated...\n");

		num_ioq_vectors = oct->num_msix_irqs;

		for (i = 0; i < num_ioq_vectors; i++) {
			irqret = request_irq(msix_entries[i].vector,
					     liquidio_msix_intr_handler, 0,
					     "octeon", &oct->ioq_vector[i]);
			if (irqret) {
				dev_err(&oct->pci_dev->dev,
					"OCTEON: Request_irq failed for MSIX interrupt Error: %d\n",
					irqret);

				while (i) {
					i--;
					irq_set_affinity_hint(
					    msix_entries[i].vector, NULL);
					free_irq(msix_entries[i].vector,
						 &oct->ioq_vector[i]);
				}
				pci_disable_msix(oct->pci_dev);
				kfree(oct->msix_entries);
				oct->msix_entries = NULL;
				return 1;
			}
			oct->ioq_vector[i].vector = msix_entries[i].vector;
			/* assign the cpu mask for this msix interrupt vector */
			irq_set_affinity_hint(
			    msix_entries[i].vector,
			    (&oct->ioq_vector[i].affinity_mask));
		}
		dev_dbg(&oct->pci_dev->dev,
			"OCTEON[%d]: MSI-X enabled\n", oct->octeon_id);
	}
	return 0;
}

/**
 * \brief PCI probe handler
 * @param pdev PCI device structure
 * @param ent unused
 */
static int
liquidio_vf_probe(struct pci_dev *pdev,
		  const struct pci_device_id *ent __attribute__((unused)))
{
	struct octeon_device *oct_dev = NULL;

	oct_dev = octeon_allocate_device(pdev->device,
					 sizeof(struct octeon_device_priv));

	if (!oct_dev) {
		dev_err(&pdev->dev, "Unable to allocate device\n");
		return -ENOMEM;
	}
	oct_dev->msix_on = LIO_FLAG_MSIX_ENABLED;

	dev_info(&pdev->dev, "Initializing device %x:%x.\n",
		 (u32)pdev->vendor, (u32)pdev->device);

	/* Assign octeon_device for this device to the private data area. */
	pci_set_drvdata(pdev, oct_dev);

	/* set linux specific device pointer */
	oct_dev->pci_dev = pdev;

	if (octeon_device_init(oct_dev)) {
		liquidio_vf_remove(pdev);
		return -ENOMEM;
	}

	dev_dbg(&oct_dev->pci_dev->dev, "Device is ready\n");

	return 0;
}

/**
 * \brief PCI FLR for each Octeon device.
 * @param oct octeon device
 */
static void octeon_pci_flr(struct octeon_device *oct)
{
	u16 status;

	pci_save_state(oct->pci_dev);

	pci_cfg_access_lock(oct->pci_dev);

	/* Quiesce the device completely */
	pci_write_config_word(oct->pci_dev, PCI_COMMAND,
			      PCI_COMMAND_INTX_DISABLE);

	/* Wait for Transaction Pending bit clean */
	msleep(100);
	pcie_capability_read_word(oct->pci_dev, PCI_EXP_DEVSTA, &status);
	if (status & PCI_EXP_DEVSTA_TRPND) {
		dev_info(&oct->pci_dev->dev, "Function reset incomplete after 100ms, sleeping for 5 seconds\n");
		ssleep(5);
		pcie_capability_read_word(oct->pci_dev, PCI_EXP_DEVSTA,
					  &status);
		if (status & PCI_EXP_DEVSTA_TRPND)
			dev_info(&oct->pci_dev->dev, "Function reset still incomplete after 5s, reset anyway\n");
	}
	pcie_capability_set_word(oct->pci_dev, PCI_EXP_DEVCTL,
				 PCI_EXP_DEVCTL_BCR_FLR);
	mdelay(100);

	pci_cfg_access_unlock(oct->pci_dev);

	pci_restore_state(oct->pci_dev);
}

/**
 *\brief Destroy resources associated with octeon device
 * @param pdev PCI device structure
 * @param ent unused
 */
static void octeon_destroy_resources(struct octeon_device *oct)
{
	struct msix_entry *msix_entries;
	int i;

	switch (atomic_read(&oct->status)) {
	case OCT_DEV_RUNNING:
	case OCT_DEV_CORE_OK:
		/* No more instructions will be forwarded. */
		atomic_set(&oct->status, OCT_DEV_IN_RESET);

		oct->app_mode = CVM_DRV_INVALID_APP;
		dev_dbg(&oct->pci_dev->dev, "Device state is now %s\n",
			lio_get_state_string(&oct->status));

		schedule_timeout_uninterruptible(HZ / 10);

		/* fallthrough */
	case OCT_DEV_HOST_OK:
		/* fallthrough */
	case OCT_DEV_IO_QUEUES_DONE:
		if (wait_for_pending_requests(oct))
			dev_err(&oct->pci_dev->dev, "There were pending requests\n");

		if (lio_wait_for_instr_fetch(oct))
			dev_err(&oct->pci_dev->dev, "IQ had pending instructions\n");

		/* Disable the input and output queues now. No more packets will
		 * arrive from Octeon, but we should wait for all packet
		 * processing to finish.
		 */
		oct->fn_list.disable_io_queues(oct);

		if (lio_wait_for_oq_pkts(oct))
			dev_err(&oct->pci_dev->dev, "OQ had pending packets\n");

	case OCT_DEV_INTR_SET_DONE:
		/* Disable interrupts  */
		oct->fn_list.disable_interrupt(oct, OCTEON_ALL_INTR);

		if (oct->msix_on) {
			msix_entries = (struct msix_entry *)oct->msix_entries;
			for (i = 0; i < oct->num_msix_irqs; i++) {
				irq_set_affinity_hint(msix_entries[i].vector,
						      NULL);
				free_irq(msix_entries[i].vector,
					 &oct->ioq_vector[i]);
			}
			pci_disable_msix(oct->pci_dev);
			kfree(oct->msix_entries);
			oct->msix_entries = NULL;
		}
		/* Soft reset the octeon device before exiting */
		if (oct->pci_dev->reset_fn)
			octeon_pci_flr(oct);
		else
			cn23xx_vf_ask_pf_to_do_flr(oct);

		/* fallthrough */
	case OCT_DEV_MSIX_ALLOC_VECTOR_DONE:
		octeon_free_ioq_vector(oct);

		/* fallthrough */
	case OCT_DEV_MBOX_SETUP_DONE:
		oct->fn_list.free_mbox(oct);

		/* fallthrough */
	case OCT_DEV_IN_RESET:
	case OCT_DEV_DROQ_INIT_DONE:
		mdelay(100);
		for (i = 0; i < MAX_OCTEON_OUTPUT_QUEUES(oct); i++) {
			if (!(oct->io_qmask.oq & BIT_ULL(i)))
				continue;
			octeon_delete_droq(oct, i);
		}

		/* fallthrough */
	case OCT_DEV_RESP_LIST_INIT_DONE:
		octeon_delete_response_list(oct);

		/* fallthrough */
	case OCT_DEV_INSTR_QUEUE_INIT_DONE:
		for (i = 0; i < MAX_OCTEON_INSTR_QUEUES(oct); i++) {
			if (!(oct->io_qmask.iq & BIT_ULL(i)))
				continue;
			octeon_delete_instr_queue(oct, i);
		}

		/* fallthrough */
	case OCT_DEV_SC_BUFF_POOL_INIT_DONE:
		octeon_free_sc_buffer_pool(oct);

		/* fallthrough */
	case OCT_DEV_DISPATCH_INIT_DONE:
		octeon_delete_dispatch_list(oct);
		cancel_delayed_work_sync(&oct->nic_poll_work.work);

		/* fallthrough */
	case OCT_DEV_PCI_MAP_DONE:
		octeon_unmap_pci_barx(oct, 0);
		octeon_unmap_pci_barx(oct, 1);

		/* fallthrough */
	case OCT_DEV_PCI_ENABLE_DONE:
		pci_clear_master(oct->pci_dev);
		/* Disable the device, releasing the PCI INT */
		pci_disable_device(oct->pci_dev);

		/* fallthrough */
	case OCT_DEV_BEGIN_STATE:
		/* Nothing to be done here either */
		break;
	}
}

/**
 * \brief Destroy NIC device interface
 * @param oct octeon device
 * @param ifidx which interface to destroy
 *
 * Cleanup associated with each interface for an Octeon device  when NIC
 * module is being unloaded or if initialization fails during load.
 */
static void liquidio_destroy_nic_device(struct octeon_device *oct, int ifidx)
{
	struct net_device *netdev = oct->props[ifidx].netdev;
	struct lio *lio;

	if (!netdev) {
		dev_err(&oct->pci_dev->dev, "%s No netdevice ptr for index %d\n",
			__func__, ifidx);
		return;
	}

	lio = GET_LIO(netdev);

	dev_dbg(&oct->pci_dev->dev, "NIC device cleanup\n");

	if (atomic_read(&lio->ifstate) & LIO_IFSTATE_RUNNING)
		liquidio_stop(netdev);

	if (atomic_read(&lio->ifstate) & LIO_IFSTATE_REGISTERED)
		unregister_netdev(netdev);

	free_netdev(netdev);

	oct->props[ifidx].gmxport = -1;

	oct->props[ifidx].netdev = NULL;
}

/**
 * \brief Stop complete NIC functionality
 * @param oct octeon device
 */
static int liquidio_stop_nic_module(struct octeon_device *oct)
{
	int i;

	dev_dbg(&oct->pci_dev->dev, "Stopping network interfaces\n");
	if (!oct->ifcount) {
		dev_err(&oct->pci_dev->dev, "Init for Octeon was not completed\n");
		return 1;
	}

	for (i = 0; i < oct->ifcount; i++)
		liquidio_destroy_nic_device(oct, i);

	dev_dbg(&oct->pci_dev->dev, "Network interfaces stopped\n");
	return 0;
}

/**
 * \brief Cleans up resources at unload time
 * @param pdev PCI device structure
 */
static void liquidio_vf_remove(struct pci_dev *pdev)
{
	struct octeon_device *oct_dev = pci_get_drvdata(pdev);

	dev_dbg(&oct_dev->pci_dev->dev, "Stopping device\n");

	if (oct_dev->app_mode == CVM_DRV_NIC_APP)
		liquidio_stop_nic_module(oct_dev);

	/* Reset the octeon device and cleanup all memory allocated for
	 * the octeon device by driver.
	 */
	octeon_destroy_resources(oct_dev);

	dev_info(&oct_dev->pci_dev->dev, "Device removed\n");

	/* This octeon device has been removed. Update the global
	 * data structure to reflect this. Free the device structure.
	 */
	octeon_free_device_mem(oct_dev);
}

/**
 * \brief PCI initialization for each Octeon device.
 * @param oct octeon device
 */
static int octeon_pci_os_setup(struct octeon_device *oct)
{
#ifdef CONFIG_PCI_IOV
	/* setup PCI stuff first */
	if (!oct->pci_dev->physfn)
		octeon_pci_flr(oct);
#endif

	if (pci_enable_device(oct->pci_dev)) {
		dev_err(&oct->pci_dev->dev, "pci_enable_device failed\n");
		return 1;
	}

	if (dma_set_mask_and_coherent(&oct->pci_dev->dev, DMA_BIT_MASK(64))) {
		dev_err(&oct->pci_dev->dev, "Unexpected DMA device capability\n");
		pci_disable_device(oct->pci_dev);
		return 1;
	}

	/* Enable PCI DMA Master. */
	pci_set_master(oct->pci_dev);

	return 0;
}

/**
 * \brief Callback for getting interface configuration
 * @param status status of request
 * @param buf pointer to resp structure
 */
static void if_cfg_callback(struct octeon_device *oct,
			    u32 status __attribute__((unused)), void *buf)
{
	struct octeon_soft_command *sc = (struct octeon_soft_command *)buf;
	struct liquidio_if_cfg_context *ctx;
	struct liquidio_if_cfg_resp *resp;

	resp = (struct liquidio_if_cfg_resp *)sc->virtrptr;
	ctx = (struct liquidio_if_cfg_context *)sc->ctxptr;

	oct = lio_get_device(ctx->octeon_id);
	if (resp->status)
		dev_err(&oct->pci_dev->dev, "nic if cfg instruction failed. Status: %llx\n",
			CVM_CAST64(resp->status));
	WRITE_ONCE(ctx->cond, 1);

	snprintf(oct->fw_info.liquidio_firmware_version, 32, "%s",
		 resp->cfg_info.liquidio_firmware_version);

	/* This barrier is required to be sure that the response has been
	 * written fully before waking up the handler
	 */
	wmb();

	wake_up_interruptible(&ctx->wc);
}

/**
 * \brief Select queue based on hash
 * @param dev Net device
 * @param skb sk_buff structure
 * @returns selected queue number
 */
static u16 select_q(struct net_device *dev, struct sk_buff *skb,
		    void *accel_priv __attribute__((unused)),
		    select_queue_fallback_t fallback __attribute__((unused)))
{
	struct lio *lio;
	u32 qindex;

	lio = GET_LIO(dev);

	qindex = skb_tx_hash(dev, skb);

	return (u16)(qindex % (lio->linfo.num_txpciq));
}

/**
 * \brief Net device stop for LiquidIO
 * @param netdev network device
 */
static int liquidio_stop(struct net_device *netdev)
{
	struct lio *lio = GET_LIO(netdev);
	struct octeon_device *oct = lio->oct_dev;

	netif_info(lio, ifdown, lio->netdev, "Stopping interface!\n");
	/* Inform that netif carrier is down */
	lio->intf_open = 0;
	lio->linfo.link.s.link_up = 0;

	netif_carrier_off(netdev);
	lio->link_changes++;

	ifstate_reset(lio, LIO_IFSTATE_RUNNING);

	dev_info(&oct->pci_dev->dev, "%s interface is stopped\n", netdev->name);

	return 0;
}

/** Sending command to enable/disable RX checksum offload
 * @param netdev                pointer to network device
 * @param command               OCTNET_CMD_TNL_RX_CSUM_CTL
 * @param rx_cmd_bit            OCTNET_CMD_RXCSUM_ENABLE/
 *                              OCTNET_CMD_RXCSUM_DISABLE
 * @returns                     SUCCESS or FAILURE
 */
static int liquidio_set_rxcsum_command(struct net_device *netdev, int command,
				       u8 rx_cmd)
{
	struct lio *lio = GET_LIO(netdev);
	struct octeon_device *oct = lio->oct_dev;
	struct octnic_ctrl_pkt nctrl;
	int ret = 0;

	nctrl.ncmd.u64 = 0;
	nctrl.ncmd.s.cmd = command;
	nctrl.ncmd.s.param1 = rx_cmd;
	nctrl.iq_no = lio->linfo.txpciq[0].s.q_no;
	nctrl.wait_time = 100;
	nctrl.netpndev = (u64)netdev;
	nctrl.cb_fn = liquidio_link_ctrl_cmd_completion;

	ret = octnet_send_nic_ctrl_pkt(lio->oct_dev, &nctrl);
	if (ret < 0) {
		dev_err(&oct->pci_dev->dev, "DEVFLAGS RXCSUM change failed in core (ret:0x%x)\n",
			ret);
	}
	return ret;
}

/** \brief Net device fix features
 * @param netdev  pointer to network device
 * @param request features requested
 * @returns updated features list
 */
static netdev_features_t liquidio_fix_features(struct net_device *netdev,
					       netdev_features_t request)
{
	struct lio *lio = netdev_priv(netdev);

	if ((request & NETIF_F_RXCSUM) &&
	    !(lio->dev_capability & NETIF_F_RXCSUM))
		request &= ~NETIF_F_RXCSUM;

	if ((request & NETIF_F_HW_CSUM) &&
	    !(lio->dev_capability & NETIF_F_HW_CSUM))
		request &= ~NETIF_F_HW_CSUM;

	if ((request & NETIF_F_TSO) && !(lio->dev_capability & NETIF_F_TSO))
		request &= ~NETIF_F_TSO;

	if ((request & NETIF_F_TSO6) && !(lio->dev_capability & NETIF_F_TSO6))
		request &= ~NETIF_F_TSO6;

	if ((request & NETIF_F_LRO) && !(lio->dev_capability & NETIF_F_LRO))
		request &= ~NETIF_F_LRO;

	/* Disable LRO if RXCSUM is off */
	if (!(request & NETIF_F_RXCSUM) && (netdev->features & NETIF_F_LRO) &&
	    (lio->dev_capability & NETIF_F_LRO))
		request &= ~NETIF_F_LRO;

	return request;
}

/** \brief Net device set features
 * @param netdev  pointer to network device
 * @param features features to enable/disable
 */
static int liquidio_set_features(struct net_device *netdev,
				 netdev_features_t features)
{
	struct lio *lio = netdev_priv(netdev);

	if (!((netdev->features ^ features) & NETIF_F_LRO))
		return 0;

	if ((features & NETIF_F_LRO) && (lio->dev_capability & NETIF_F_LRO))
		liquidio_set_feature(netdev, OCTNET_CMD_LRO_ENABLE,
				     OCTNIC_LROIPV4 | OCTNIC_LROIPV6);
	else if (!(features & NETIF_F_LRO) &&
		 (lio->dev_capability & NETIF_F_LRO))
		liquidio_set_feature(netdev, OCTNET_CMD_LRO_DISABLE,
				     OCTNIC_LROIPV4 | OCTNIC_LROIPV6);
	if (!(netdev->features & NETIF_F_RXCSUM) &&
	    (lio->enc_dev_capability & NETIF_F_RXCSUM) &&
	    (features & NETIF_F_RXCSUM))
		liquidio_set_rxcsum_command(netdev, OCTNET_CMD_TNL_RX_CSUM_CTL,
					    OCTNET_CMD_RXCSUM_ENABLE);
	else if ((netdev->features & NETIF_F_RXCSUM) &&
		 (lio->enc_dev_capability & NETIF_F_RXCSUM) &&
		 !(features & NETIF_F_RXCSUM))
		liquidio_set_rxcsum_command(netdev, OCTNET_CMD_TNL_RX_CSUM_CTL,
					    OCTNET_CMD_RXCSUM_DISABLE);

	return 0;
}

static const struct net_device_ops lionetdevops = {
	.ndo_fix_features	= liquidio_fix_features,
	.ndo_set_features	= liquidio_set_features,
	.ndo_select_queue	= select_q,
};

/**
 * \brief Setup network interfaces
 * @param octeon_dev  octeon device
 *
 * Called during init time for each device. It assumes the NIC
 * is already up and running.  The link information for each
 * interface is passed in link_info.
 */
static int setup_nic_devices(struct octeon_device *octeon_dev)
{
	int retval, num_iqueues, num_oqueues;
	struct liquidio_if_cfg_context *ctx;
	u32 resp_size, ctx_size, data_size;
	struct liquidio_if_cfg_resp *resp;
	struct octeon_soft_command *sc;
	union oct_nic_if_cfg if_cfg;
	struct octdev_props *props;
	struct net_device *netdev;
	struct lio_version *vdata;
	struct lio *lio = NULL;
	u8 mac[ETH_ALEN], i, j;
	u32 ifidx_or_pfnum;

	ifidx_or_pfnum = octeon_dev->pf_num;

	for (i = 0; i < octeon_dev->ifcount; i++) {
		resp_size = sizeof(struct liquidio_if_cfg_resp);
		ctx_size = sizeof(struct liquidio_if_cfg_context);
		data_size = sizeof(struct lio_version);
		sc = (struct octeon_soft_command *)
			octeon_alloc_soft_command(octeon_dev, data_size,
						  resp_size, ctx_size);
		resp = (struct liquidio_if_cfg_resp *)sc->virtrptr;
		ctx  = (struct liquidio_if_cfg_context *)sc->ctxptr;
		vdata = (struct lio_version *)sc->virtdptr;

		*((u64 *)vdata) = 0;
		vdata->major = cpu_to_be16(LIQUIDIO_BASE_MAJOR_VERSION);
		vdata->minor = cpu_to_be16(LIQUIDIO_BASE_MINOR_VERSION);
		vdata->micro = cpu_to_be16(LIQUIDIO_BASE_MICRO_VERSION);

		WRITE_ONCE(ctx->cond, 0);
		ctx->octeon_id = lio_get_device_id(octeon_dev);
		init_waitqueue_head(&ctx->wc);

		if_cfg.u64 = 0;

		if_cfg.s.num_iqueues = octeon_dev->sriov_info.rings_per_vf;
		if_cfg.s.num_oqueues = octeon_dev->sriov_info.rings_per_vf;
		if_cfg.s.base_queue = 0;

		sc->iq_no = 0;

		octeon_prepare_soft_command(octeon_dev, sc, OPCODE_NIC,
					    OPCODE_NIC_IF_CFG, 0, if_cfg.u64,
					    0);

		sc->callback = if_cfg_callback;
		sc->callback_arg = sc;
		sc->wait_time = 5000;

		retval = octeon_send_soft_command(octeon_dev, sc);
		if (retval == IQ_SEND_FAILED) {
			dev_err(&octeon_dev->pci_dev->dev,
				"iq/oq config failed status: %x\n", retval);
			/* Soft instr is freed by driver in case of failure. */
			goto setup_nic_dev_fail;
		}

		/* Sleep on a wait queue till the cond flag indicates that the
		 * response arrived or timed-out.
		 */
		if (sleep_cond(&ctx->wc, &ctx->cond) == -EINTR) {
			dev_err(&octeon_dev->pci_dev->dev, "Wait interrupted\n");
			goto setup_nic_wait_intr;
		}

		retval = resp->status;
		if (retval) {
			dev_err(&octeon_dev->pci_dev->dev, "iq/oq config failed\n");
			goto setup_nic_dev_fail;
		}

		octeon_swap_8B_data((u64 *)(&resp->cfg_info),
				    (sizeof(struct liquidio_if_cfg_info)) >> 3);

		num_iqueues = hweight64(resp->cfg_info.iqmask);
		num_oqueues = hweight64(resp->cfg_info.oqmask);

		if (!(num_iqueues) || !(num_oqueues)) {
			dev_err(&octeon_dev->pci_dev->dev,
				"Got bad iqueues (%016llx) or oqueues (%016llx) from firmware.\n",
				resp->cfg_info.iqmask, resp->cfg_info.oqmask);
			goto setup_nic_dev_fail;
		}
		dev_dbg(&octeon_dev->pci_dev->dev,
			"interface %d, iqmask %016llx, oqmask %016llx, numiqueues %d, numoqueues %d\n",
			i, resp->cfg_info.iqmask, resp->cfg_info.oqmask,
			num_iqueues, num_oqueues);

		netdev = alloc_etherdev_mq(LIO_SIZE, num_iqueues);

		if (!netdev) {
			dev_err(&octeon_dev->pci_dev->dev, "Device allocation failed\n");
			goto setup_nic_dev_fail;
		}

		SET_NETDEV_DEV(netdev, &octeon_dev->pci_dev->dev);

		/* Associate the routines that will handle different
		 * netdev tasks.
		 */
		netdev->netdev_ops = &lionetdevops;

		lio = GET_LIO(netdev);

		memset(lio, 0, sizeof(struct lio));

		lio->ifidx = ifidx_or_pfnum;

		props = &octeon_dev->props[i];
		props->gmxport = resp->cfg_info.linfo.gmxport;
		props->netdev = netdev;

		lio->linfo.num_rxpciq = num_oqueues;
		lio->linfo.num_txpciq = num_iqueues;

		for (j = 0; j < num_oqueues; j++) {
			lio->linfo.rxpciq[j].u64 =
			    resp->cfg_info.linfo.rxpciq[j].u64;
		}
		for (j = 0; j < num_iqueues; j++) {
			lio->linfo.txpciq[j].u64 =
			    resp->cfg_info.linfo.txpciq[j].u64;
		}

		lio->linfo.hw_addr = resp->cfg_info.linfo.hw_addr;
		lio->linfo.gmxport = resp->cfg_info.linfo.gmxport;
		lio->linfo.link.u64 = resp->cfg_info.linfo.link.u64;
		lio->linfo.macaddr_is_admin_asgnd =
			resp->cfg_info.linfo.macaddr_is_admin_asgnd;

		lio->msg_enable = netif_msg_init(debug, DEFAULT_MSG_ENABLE);

		lio->dev_capability = NETIF_F_HIGHDMA
				      | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM
				      | NETIF_F_SG | NETIF_F_RXCSUM
				      | NETIF_F_TSO | NETIF_F_TSO6
				      | NETIF_F_GRO
				      | NETIF_F_LRO;
		netif_set_gso_max_size(netdev, OCTNIC_GSO_MAX_SIZE);

		netdev->features = (lio->dev_capability & ~NETIF_F_LRO);

		netdev->hw_features = lio->dev_capability;

		/* Point to the  properties for octeon device to which this
		 * interface belongs.
		 */
		lio->oct_dev = octeon_dev;
		lio->octprops = props;
		lio->netdev = netdev;

		dev_dbg(&octeon_dev->pci_dev->dev,
			"if%d gmx: %d hw_addr: 0x%llx\n", i,
			lio->linfo.gmxport, CVM_CAST64(lio->linfo.hw_addr));

		/* 64-bit swap required on LE machines */
		octeon_swap_8B_data(&lio->linfo.hw_addr, 1);
		for (j = 0; j < ETH_ALEN; j++)
			mac[j] = *((u8 *)(((u8 *)&lio->linfo.hw_addr) + 2 + j));

		/* Copy MAC Address to OS network device structure */
		ether_addr_copy(netdev->dev_addr, mac);

		if (netdev->features & NETIF_F_LRO)
			liquidio_set_feature(netdev, OCTNET_CMD_LRO_ENABLE,
					     OCTNIC_LROIPV4 | OCTNIC_LROIPV6);

		if ((debug != -1) && (debug & NETIF_MSG_HW))
			liquidio_set_feature(netdev, OCTNET_CMD_VERBOSE_ENABLE,
					     0);

		/* Register the network device with the OS */
		if (register_netdev(netdev)) {
			dev_err(&octeon_dev->pci_dev->dev, "Device registration failed\n");
			goto setup_nic_dev_fail;
		}

		dev_dbg(&octeon_dev->pci_dev->dev,
			"Setup NIC ifidx:%d mac:%02x%02x%02x%02x%02x%02x\n",
			i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		netif_carrier_off(netdev);
		lio->link_changes++;

		ifstate_set(lio, LIO_IFSTATE_REGISTERED);

		/* Sending command to firmware to enable Rx checksum offload
		 * by default at the time of setup of Liquidio driver for
		 * this device
		 */
		liquidio_set_rxcsum_command(netdev, OCTNET_CMD_TNL_RX_CSUM_CTL,
					    OCTNET_CMD_RXCSUM_ENABLE);
		liquidio_set_feature(netdev, OCTNET_CMD_TNL_TX_CSUM_CTL,
				     OCTNET_CMD_TXCSUM_ENABLE);

		dev_dbg(&octeon_dev->pci_dev->dev,
			"NIC ifidx:%d Setup successful\n", i);

		octeon_free_soft_command(octeon_dev, sc);
	}

	return 0;

setup_nic_dev_fail:

	octeon_free_soft_command(octeon_dev, sc);

setup_nic_wait_intr:

	while (i--) {
		dev_err(&octeon_dev->pci_dev->dev,
			"NIC ifidx:%d Setup failed\n", i);
		liquidio_destroy_nic_device(octeon_dev, i);
	}
	return -ENODEV;
}

/**
 * \brief initialize the NIC
 * @param oct octeon device
 *
 * This initialization routine is called once the Octeon device application is
 * up and running
 */
static int liquidio_init_nic_module(struct octeon_device *oct)
{
	int num_nic_ports = 1;
	int i, retval = 0;

	dev_dbg(&oct->pci_dev->dev, "Initializing network interfaces\n");

	/* only default iq and oq were initialized
	 * initialize the rest as well run port_config command for each port
	 */
	oct->ifcount = num_nic_ports;
	memset(oct->props, 0,
	       sizeof(struct octdev_props) * num_nic_ports);

	for (i = 0; i < MAX_OCTEON_LINKS; i++)
		oct->props[i].gmxport = -1;

	retval = setup_nic_devices(oct);
	if (retval) {
		dev_err(&oct->pci_dev->dev, "Setup NIC devices failed\n");
		goto octnet_init_failure;
	}

octnet_init_failure:

	oct->ifcount = 0;

	return retval;
}

/**
 * \brief Device initialization for each Octeon device that is probed
 * @param octeon_dev  octeon device
 */
static int octeon_device_init(struct octeon_device *oct)
{
	u32 rev_id;
	int j;

	atomic_set(&oct->status, OCT_DEV_BEGIN_STATE);

	/* Enable access to the octeon device and make its DMA capability
	 * known to the OS.
	 */
	if (octeon_pci_os_setup(oct))
		return 1;
	atomic_set(&oct->status, OCT_DEV_PCI_ENABLE_DONE);

	oct->chip_id = OCTEON_CN23XX_VF_VID;
	pci_read_config_dword(oct->pci_dev, 8, &rev_id);
	oct->rev_id = rev_id & 0xff;

	if (cn23xx_setup_octeon_vf_device(oct))
		return 1;

	atomic_set(&oct->status, OCT_DEV_PCI_MAP_DONE);

	oct->app_mode = CVM_DRV_NIC_APP;

	/* Initialize the dispatch mechanism used to push packets arriving on
	 * Octeon Output queues.
	 */
	if (octeon_init_dispatch_list(oct))
		return 1;

	atomic_set(&oct->status, OCT_DEV_DISPATCH_INIT_DONE);

	if (octeon_set_io_queues_off(oct)) {
		dev_err(&oct->pci_dev->dev, "setting io queues off failed\n");
		return 1;
	}

	if (oct->fn_list.setup_device_regs(oct)) {
		dev_err(&oct->pci_dev->dev, "device registers configuration failed\n");
		return 1;
	}

	/* Initialize soft command buffer pool */
	if (octeon_setup_sc_buffer_pool(oct)) {
		dev_err(&oct->pci_dev->dev, "sc buffer pool allocation failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_SC_BUFF_POOL_INIT_DONE);

	/* Setup the data structures that manage this Octeon's Input queues. */
	if (octeon_setup_instr_queues(oct)) {
		dev_err(&oct->pci_dev->dev, "instruction queue initialization failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_INSTR_QUEUE_INIT_DONE);

	/* Initialize lists to manage the requests of different types that
	 * arrive from user & kernel applications for this octeon device.
	 */
	if (octeon_setup_response_list(oct)) {
		dev_err(&oct->pci_dev->dev, "Response list allocation failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_RESP_LIST_INIT_DONE);

	if (octeon_setup_output_queues(oct)) {
		dev_err(&oct->pci_dev->dev, "Output queue initialization failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_DROQ_INIT_DONE);

	if (oct->fn_list.setup_mbox(oct)) {
		dev_err(&oct->pci_dev->dev, "Mailbox setup failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_MBOX_SETUP_DONE);

	if (octeon_allocate_ioq_vector(oct)) {
		dev_err(&oct->pci_dev->dev, "ioq vector allocation failed\n");
		return 1;
	}
	atomic_set(&oct->status, OCT_DEV_MSIX_ALLOC_VECTOR_DONE);

	dev_info(&oct->pci_dev->dev, "OCTEON_CN23XX VF Version: %s, %d ioqs\n",
		 LIQUIDIO_VERSION, oct->sriov_info.rings_per_vf);

	/* Setup the interrupt handler and record the INT SUM register address*/
	if (octeon_setup_interrupt(oct))
		return 1;

	if (cn23xx_octeon_pfvf_handshake(oct))
		return 1;

	/* Enable Octeon device interrupts */
	oct->fn_list.enable_interrupt(oct, OCTEON_ALL_INTR);

	atomic_set(&oct->status, OCT_DEV_INTR_SET_DONE);

	/* Enable the input and output queues for this Octeon device */
	if (oct->fn_list.enable_io_queues(oct)) {
		dev_err(&oct->pci_dev->dev, "enabling io queues failed\n");
		return 1;
	}

	atomic_set(&oct->status, OCT_DEV_IO_QUEUES_DONE);

	atomic_set(&oct->status, OCT_DEV_HOST_OK);

	/* Send Credit for Octeon Output queues. Credits are always sent after
	 * the output queue is enabled.
	 */
	for (j = 0; j < oct->num_oqs; j++)
		writel(oct->droq[j]->max_count, oct->droq[j]->pkts_credit_reg);

	/* Packets can start arriving on the output queues from this point. */

	atomic_set(&oct->status, OCT_DEV_CORE_OK);

	atomic_set(&oct->status, OCT_DEV_RUNNING);

	if (liquidio_init_nic_module(oct))
		return 1;

	return 0;
}

static int __init liquidio_vf_init(void)
{
	octeon_init_device_list(0);
	return pci_register_driver(&liquidio_vf_pci_driver);
}

static void __exit liquidio_vf_exit(void)
{
	pci_unregister_driver(&liquidio_vf_pci_driver);

	pr_info("LiquidIO_VF network module is now unloaded\n");
}

module_init(liquidio_vf_init);
module_exit(liquidio_vf_exit);
