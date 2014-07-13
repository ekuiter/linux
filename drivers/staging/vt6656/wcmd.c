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
 * File: wcmd.c
 *
 * Purpose: Handles the management command interface functions
 *
 * Author: Lyndon Chen
 *
 * Date: May 8, 2003
 *
 * Functions:
 *      s_vProbeChannel - Active scan channel
 *      s_MgrMakeProbeRequest - Make ProbeRequest packet
 *      CommandTimer - Timer function to handle command
 *	vnt_cmd_complete - Command Complete function
 *      bScheduleCommand - Push Command and wait Command Scheduler to do
 *      vCommandTimer- Command call back functions
 *	vnt_cmd_timer_wait- Call back timer
 *      s_bClearBSSID_SCAN- Clear BSSID_SCAN cmd in CMD Queue
 *
 * Revision History:
 *
 */

#include "device.h"
#include "mac.h"
#include "card.h"
#include "wcmd.h"
#include "power.h"
#include "baseband.h"
#include "usbpipe.h"
#include "rxtx.h"
#include "rf.h"
#include "channel.h"

static void vnt_cmd_timer_wait(struct vnt_private *priv, unsigned long msecs)
{
	schedule_delayed_work(&priv->run_command_work, msecs_to_jiffies(msecs));
}

static int vnt_cmd_complete(struct vnt_private *priv)
{

	priv->command_state = WLAN_CMD_IDLE;
	if (priv->free_cmd_queue == CMD_Q_SIZE) {
		/* Command Queue Empty */
		priv->cmd_running = false;
		return true;
	}

	priv->command = priv->cmd_queue[priv->cmd_dequeue_idx];

	ADD_ONE_WITH_WRAP_AROUND(priv->cmd_dequeue_idx, CMD_Q_SIZE);
	priv->free_cmd_queue++;
	priv->cmd_running = true;

	switch (priv->command) {
	case WLAN_CMD_INIT_MAC80211:
		priv->command_state = WLAN_CMD_INIT_MAC80211_START;
		break;

	case WLAN_CMD_TBTT_WAKEUP:
		priv->command_state = WLAN_CMD_TBTT_WAKEUP_START;
		break;

	case WLAN_CMD_BECON_SEND:
		priv->command_state = WLAN_CMD_BECON_SEND_START;
		break;

	case WLAN_CMD_SETPOWER:
		priv->command_state = WLAN_CMD_SETPOWER_START;
		break;

	case WLAN_CMD_CHANGE_ANTENNA:
		priv->command_state = WLAN_CMD_CHANGE_ANTENNA_START;
		break;

	case WLAN_CMD_11H_CHSW:
		priv->command_state = WLAN_CMD_11H_CHSW_START;
		break;

	default:
		break;
	}

	vnt_cmd_timer_wait(priv, 0);

	return true;
}

void vRunCommand(struct work_struct *work)
{
	struct vnt_private *priv =
		container_of(work, struct vnt_private, run_command_work.work);

	if (priv->Flags & fMP_DISCONNECTED)
		return;

	if (priv->cmd_running != true)
		return;

	switch (priv->command_state) {
	case WLAN_CMD_INIT_MAC80211_START:
		if (priv->mac_hw)
			break;

		dev_info(&priv->usb->dev, "Starting mac80211\n");

		if (vnt_init(priv)) {
			/* If fail all ends TODO retry */
			dev_err(&priv->usb->dev, "failed to start\n");
			ieee80211_free_hw(priv->hw);
			return;
		}

		break;

	case WLAN_CMD_TBTT_WAKEUP_START:
		vnt_next_tbtt_wakeup(priv);
		break;

	case WLAN_CMD_BECON_SEND_START:
		if (!priv->vif)
			break;

		vnt_beacon_make(priv, priv->vif);

		vnt_mac_reg_bits_on(priv, MAC_REG_TCR, TCR_AUTOBCNTX);

		break;

	case WLAN_CMD_SETPOWER_START:

		vnt_rf_setpower(priv, priv->wCurrentRate,
				priv->hw->conf.chandef.chan->hw_value);

		break;

	case WLAN_CMD_CHANGE_ANTENNA_START:
		dev_dbg(&priv->usb->dev, "Change from Antenna%d to",
							priv->dwRxAntennaSel);

		if (priv->dwRxAntennaSel == 0) {
			priv->dwRxAntennaSel = 1;
			if (priv->bTxRxAntInv == true)
				BBvSetAntennaMode(priv, ANT_RXA);
			else
				BBvSetAntennaMode(priv, ANT_RXB);
		} else {
			priv->dwRxAntennaSel = 0;
			if (priv->bTxRxAntInv == true)
				BBvSetAntennaMode(priv, ANT_RXB);
			else
				BBvSetAntennaMode(priv, ANT_RXA);
		}
		break;

	case WLAN_CMD_11H_CHSW_START:
		vnt_set_channel(priv, priv->hw->conf.chandef.chan->hw_value);
		break;

	default:
		break;
	} //switch

	vnt_cmd_complete(priv);

	return;
}

int bScheduleCommand(struct vnt_private *priv, enum vnt_cmd command, u8 *item0)
{

	if (priv->free_cmd_queue == 0)
		return false;

	priv->cmd_queue[priv->cmd_enqueue_idx] = command;

	ADD_ONE_WITH_WRAP_AROUND(priv->cmd_enqueue_idx, CMD_Q_SIZE);
	priv->free_cmd_queue--;

	if (priv->cmd_running == false)
		vnt_cmd_complete(priv);

	return true;

}

void vResetCommandTimer(struct vnt_private *priv)
{
	priv->free_cmd_queue = CMD_Q_SIZE;
	priv->cmd_dequeue_idx = 0;
	priv->cmd_enqueue_idx = 0;
	priv->command_state = WLAN_CMD_IDLE;
	priv->cmd_running = false;
}
