/* Intel Ethernet Switch Host Interface Driver
 * Copyright(c) 2013 - 2014 Intel Corporation.
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
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 */

#include "fm10k.h"

static netdev_tx_t fm10k_xmit_frame(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int fm10k_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu < 68 || new_mtu > FM10K_MAX_JUMBO_FRAME_SIZE)
		return -EINVAL;

	dev->mtu = new_mtu;

	return 0;
}

static int fm10k_uc_vlan_unsync(struct net_device *netdev,
				const unsigned char *uc_addr)
{
	struct fm10k_intfc *interface = netdev_priv(netdev);
	struct fm10k_hw *hw = &interface->hw;
	u16 glort = interface->glort;
	u16 vid = interface->vid;
	bool set = !!(vid / VLAN_N_VID);
	int err;

	/* drop any leading bits on the VLAN ID */
	vid &= VLAN_N_VID - 1;

	err = hw->mac.ops.update_uc_addr(hw, glort, uc_addr, vid, set, 0);
	if (err)
		return err;

	/* return non-zero value as we are only doing a partial sync/unsync */
	return 1;
}

static int fm10k_mc_vlan_unsync(struct net_device *netdev,
				const unsigned char *mc_addr)
{
	struct fm10k_intfc *interface = netdev_priv(netdev);
	struct fm10k_hw *hw = &interface->hw;
	u16 glort = interface->glort;
	u16 vid = interface->vid;
	bool set = !!(vid / VLAN_N_VID);
	int err;

	/* drop any leading bits on the VLAN ID */
	vid &= VLAN_N_VID - 1;

	err = hw->mac.ops.update_mc_addr(hw, glort, mc_addr, vid, set);
	if (err)
		return err;

	/* return non-zero value as we are only doing a partial sync/unsync */
	return 1;
}

static int fm10k_update_vid(struct net_device *netdev, u16 vid, bool set)
{
	struct fm10k_intfc *interface = netdev_priv(netdev);
	struct fm10k_hw *hw = &interface->hw;
	s32 err;

	/* updates do not apply to VLAN 0 */
	if (!vid)
		return 0;

	if (vid >= VLAN_N_VID)
		return -EINVAL;

	/* Verify we have permission to add VLANs */
	if (hw->mac.vlan_override)
		return -EACCES;

	/* if default VLAN is already present do nothing */
	if (vid == hw->mac.default_vid)
		return -EBUSY;

	/* update active_vlans bitmask */
	set_bit(vid, interface->active_vlans);
	if (!set)
		clear_bit(vid, interface->active_vlans);

	fm10k_mbx_lock(interface);

	/* only need to update the VLAN if not in promiscous mode */
	if (!(netdev->flags & IFF_PROMISC)) {
		err = hw->mac.ops.update_vlan(hw, vid, 0, set);
		if (err)
			return err;
	}

	/* update our base MAC address */
	err = hw->mac.ops.update_uc_addr(hw, interface->glort, hw->mac.addr,
					 vid, set, 0);
	if (err)
		return err;

	/* set vid prior to syncing/unsyncing the VLAN */
	interface->vid = vid + (set ? VLAN_N_VID : 0);

	/* Update the unicast and multicast address list to add/drop VLAN */
	__dev_uc_unsync(netdev, fm10k_uc_vlan_unsync);
	__dev_mc_unsync(netdev, fm10k_mc_vlan_unsync);

	fm10k_mbx_unlock(interface);

	return 0;
}

static int fm10k_vlan_rx_add_vid(struct net_device *netdev,
				 __always_unused __be16 proto, u16 vid)
{
	/* update VLAN and address table based on changes */
	return fm10k_update_vid(netdev, vid, true);
}

static int fm10k_vlan_rx_kill_vid(struct net_device *netdev,
				  __always_unused __be16 proto, u16 vid)
{
	/* update VLAN and address table based on changes */
	return fm10k_update_vid(netdev, vid, false);
}

static u16 fm10k_find_next_vlan(struct fm10k_intfc *interface, u16 vid)
{
	struct fm10k_hw *hw = &interface->hw;
	u16 default_vid = hw->mac.default_vid;
	u16 vid_limit = vid < default_vid ? default_vid : VLAN_N_VID;

	vid = find_next_bit(interface->active_vlans, vid_limit, ++vid);

	return vid;
}

static void fm10k_clear_unused_vlans(struct fm10k_intfc *interface)
{
	struct fm10k_hw *hw = &interface->hw;
	u32 vid, prev_vid;

	/* loop through and find any gaps in the table */
	for (vid = 0, prev_vid = 0;
	     prev_vid < VLAN_N_VID;
	     prev_vid = vid + 1, vid = fm10k_find_next_vlan(interface, vid)) {
		if (prev_vid == vid)
			continue;

		/* send request to clear multiple bits at a time */
		prev_vid += (vid - prev_vid - 1) << FM10K_VLAN_LENGTH_SHIFT;
		hw->mac.ops.update_vlan(hw, prev_vid, 0, false);
	}
}

static int __fm10k_uc_sync(struct net_device *dev,
			   const unsigned char *addr, bool sync)
{
	struct fm10k_intfc *interface = netdev_priv(dev);
	struct fm10k_hw *hw = &interface->hw;
	u16 vid, glort = interface->glort;
	s32 err;

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	/* update table with current entries */
	for (vid = hw->mac.default_vid ? fm10k_find_next_vlan(interface, 0) : 0;
	     vid < VLAN_N_VID;
	     vid = fm10k_find_next_vlan(interface, vid)) {
		err = hw->mac.ops.update_uc_addr(hw, glort, addr,
						  vid, sync, 0);
		if (err)
			return err;
	}

	return 0;
}

static int fm10k_uc_sync(struct net_device *dev,
			 const unsigned char *addr)
{
	return __fm10k_uc_sync(dev, addr, true);
}

static int fm10k_uc_unsync(struct net_device *dev,
			   const unsigned char *addr)
{
	return __fm10k_uc_sync(dev, addr, false);
}

static int fm10k_set_mac(struct net_device *dev, void *p)
{
	struct fm10k_intfc *interface = netdev_priv(dev);
	struct fm10k_hw *hw = &interface->hw;
	struct sockaddr *addr = p;
	s32 err = 0;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	if (dev->flags & IFF_UP) {
		/* setting MAC address requires mailbox */
		fm10k_mbx_lock(interface);

		err = fm10k_uc_sync(dev, addr->sa_data);
		if (!err)
			fm10k_uc_unsync(dev, hw->mac.addr);

		fm10k_mbx_unlock(interface);
	}

	if (!err) {
		ether_addr_copy(dev->dev_addr, addr->sa_data);
		ether_addr_copy(hw->mac.addr, addr->sa_data);
		dev->addr_assign_type &= ~NET_ADDR_RANDOM;
	}

	/* if we had a mailbox error suggest trying again */
	return err ? -EAGAIN : 0;
}

static int __fm10k_mc_sync(struct net_device *dev,
			   const unsigned char *addr, bool sync)
{
	struct fm10k_intfc *interface = netdev_priv(dev);
	struct fm10k_hw *hw = &interface->hw;
	u16 vid, glort = interface->glort;
	s32 err;

	if (!is_multicast_ether_addr(addr))
		return -EADDRNOTAVAIL;

	/* update table with current entries */
	for (vid = hw->mac.default_vid ? fm10k_find_next_vlan(interface, 0) : 0;
	     vid < VLAN_N_VID;
	     vid = fm10k_find_next_vlan(interface, vid)) {
		err = hw->mac.ops.update_mc_addr(hw, glort, addr, vid, sync);
		if (err)
			return err;
	}

	return 0;
}

static int fm10k_mc_sync(struct net_device *dev,
			 const unsigned char *addr)
{
	return __fm10k_mc_sync(dev, addr, true);
}

static int fm10k_mc_unsync(struct net_device *dev,
			   const unsigned char *addr)
{
	return __fm10k_mc_sync(dev, addr, false);
}

static void fm10k_set_rx_mode(struct net_device *dev)
{
	struct fm10k_intfc *interface = netdev_priv(dev);
	struct fm10k_hw *hw = &interface->hw;
	int xcast_mode;

	/* no need to update the harwdare if we are not running */
	if (!(dev->flags & IFF_UP))
		return;

	/* determine new mode based on flags */
	xcast_mode = (dev->flags & IFF_PROMISC) ? FM10K_XCAST_MODE_PROMISC :
		     (dev->flags & IFF_ALLMULTI) ? FM10K_XCAST_MODE_ALLMULTI :
		     (dev->flags & (IFF_BROADCAST | IFF_MULTICAST)) ?
		     FM10K_XCAST_MODE_MULTI : FM10K_XCAST_MODE_NONE;

	fm10k_mbx_lock(interface);

	/* syncronize all of the addresses */
	if (xcast_mode != FM10K_XCAST_MODE_PROMISC) {
		__dev_uc_sync(dev, fm10k_uc_sync, fm10k_uc_unsync);
		if (xcast_mode != FM10K_XCAST_MODE_ALLMULTI)
			__dev_mc_sync(dev, fm10k_mc_sync, fm10k_mc_unsync);
	}

	/* if we aren't changing modes there is nothing to do */
	if (interface->xcast_mode != xcast_mode) {
		/* update VLAN table */
		if (xcast_mode == FM10K_XCAST_MODE_PROMISC)
			hw->mac.ops.update_vlan(hw, FM10K_VLAN_ALL, 0, true);
		if (interface->xcast_mode == FM10K_XCAST_MODE_PROMISC)
			fm10k_clear_unused_vlans(interface);

		/* update xcast mode */
		hw->mac.ops.update_xcast_mode(hw, interface->glort, xcast_mode);

		/* record updated xcast mode state */
		interface->xcast_mode = xcast_mode;
	}

	fm10k_mbx_unlock(interface);
}

void fm10k_restore_rx_state(struct fm10k_intfc *interface)
{
	struct net_device *netdev = interface->netdev;
	struct fm10k_hw *hw = &interface->hw;
	int xcast_mode;
	u16 vid, glort;

	/* record glort for this interface */
	glort = interface->glort;

	/* convert interface flags to xcast mode */
	if (netdev->flags & IFF_PROMISC)
		xcast_mode = FM10K_XCAST_MODE_PROMISC;
	else if (netdev->flags & IFF_ALLMULTI)
		xcast_mode = FM10K_XCAST_MODE_ALLMULTI;
	else if (netdev->flags & (IFF_BROADCAST | IFF_MULTICAST))
		xcast_mode = FM10K_XCAST_MODE_MULTI;
	else
		xcast_mode = FM10K_XCAST_MODE_NONE;

	fm10k_mbx_lock(interface);

	/* Enable logical port */
	hw->mac.ops.update_lport_state(hw, glort, interface->glort_count, true);

	/* update VLAN table */
	hw->mac.ops.update_vlan(hw, FM10K_VLAN_ALL, 0,
				xcast_mode == FM10K_XCAST_MODE_PROMISC);

	/* Add filter for VLAN 0 */
	hw->mac.ops.update_vlan(hw, 0, 0, true);

	/* update table with current entries */
	for (vid = hw->mac.default_vid ? fm10k_find_next_vlan(interface, 0) : 0;
	     vid < VLAN_N_VID;
	     vid = fm10k_find_next_vlan(interface, vid)) {
		hw->mac.ops.update_vlan(hw, vid, 0, true);
		hw->mac.ops.update_uc_addr(hw, glort, hw->mac.addr,
					   vid, true, 0);
	}

	/* syncronize all of the addresses */
	if (xcast_mode != FM10K_XCAST_MODE_PROMISC) {
		__dev_uc_sync(netdev, fm10k_uc_sync, fm10k_uc_unsync);
		if (xcast_mode != FM10K_XCAST_MODE_ALLMULTI)
			__dev_mc_sync(netdev, fm10k_mc_sync, fm10k_mc_unsync);
	}

	/* update xcast mode */
	hw->mac.ops.update_xcast_mode(hw, glort, xcast_mode);

	fm10k_mbx_unlock(interface);

	/* record updated xcast mode state */
	interface->xcast_mode = xcast_mode;
}

void fm10k_reset_rx_state(struct fm10k_intfc *interface)
{
	struct net_device *netdev = interface->netdev;
	struct fm10k_hw *hw = &interface->hw;

	fm10k_mbx_lock(interface);

	/* clear the logical port state on lower device */
	hw->mac.ops.update_lport_state(hw, interface->glort,
				       interface->glort_count, false);

	fm10k_mbx_unlock(interface);

	/* reset flags to default state */
	interface->xcast_mode = FM10K_XCAST_MODE_NONE;

	/* clear the sync flag since the lport has been dropped */
	__dev_uc_unsync(netdev, NULL);
	__dev_mc_unsync(netdev, NULL);
}

static const struct net_device_ops fm10k_netdev_ops = {
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_start_xmit		= fm10k_xmit_frame,
	.ndo_set_mac_address	= fm10k_set_mac,
	.ndo_change_mtu		= fm10k_change_mtu,
	.ndo_vlan_rx_add_vid	= fm10k_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= fm10k_vlan_rx_kill_vid,
	.ndo_set_rx_mode	= fm10k_set_rx_mode,
};

#define DEFAULT_DEBUG_LEVEL_SHIFT 3

struct net_device *fm10k_alloc_netdev(void)
{
	struct fm10k_intfc *interface;
	struct net_device *dev;

	dev = alloc_etherdev(sizeof(struct fm10k_intfc));
	if (!dev)
		return NULL;

	/* set net device and ethtool ops */
	dev->netdev_ops = &fm10k_netdev_ops;

	/* configure default debug level */
	interface = netdev_priv(dev);
	interface->msg_enable = (1 << DEFAULT_DEBUG_LEVEL_SHIFT) - 1;

	/* configure default features */
	dev->features |= NETIF_F_SG;

	/* all features defined to this point should be changeable */
	dev->hw_features |= dev->features;

	/* configure VLAN features */
	dev->vlan_features |= dev->features;

	/* configure tunnel offloads */
	dev->hw_enc_features = NETIF_F_SG;

	/* we want to leave these both on as we cannot disable VLAN tag
	 * insertion or stripping on the hardware since it is contained
	 * in the FTAG and not in the frame itself.
	 */
	dev->features |= NETIF_F_HW_VLAN_CTAG_TX |
			 NETIF_F_HW_VLAN_CTAG_RX |
			 NETIF_F_HW_VLAN_CTAG_FILTER;

	dev->priv_flags |= IFF_UNICAST_FLT;

	return dev;
}
