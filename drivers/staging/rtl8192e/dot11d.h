/* SPDX-License-Identifier: GPL-2.0 */
/******************************************************************************
 * Copyright(c) 2008 - 2010 Realtek Corporation. All rights reserved.
 *
 * Contact Information:
 * wlanfae <wlanfae@realtek.com>
 ******************************************************************************/
#ifndef __INC_DOT11D_H
#define __INC_DOT11D_H

#include "rtllib.h"

/**
 * struct rt_dot11d_info
 * @channel_map: holds channel values
 *		0 - invalid,
 *		1 - valid (active scan),
 *		2 - valid (passive scan)
 */

struct rt_dot11d_info {
};

#define GET_DOT11D_INFO(__ieee_dev)			\
	 ((struct rt_dot11d_info *)((__ieee_dev)->dot11d_info))

void dot11d_channel_map(struct rtllib_device *ieee);

#endif
