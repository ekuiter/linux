/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019, Silicon Laboratories, Inc.
 */
#ifndef WFX_SECURE_LINK_H
#define WFX_SECURE_LINK_H

#include "hif_api_general.h"

struct wfx_dev;


struct sl_context {
};

static inline bool wfx_is_secure_command(struct wfx_dev *wdev, int cmd_id)
{
	return false;
}

static inline int wfx_sl_decode(struct wfx_dev *wdev, struct hif_sl_msg *m)
{
	return -EIO;
}

static inline int wfx_sl_encode(struct wfx_dev *wdev, struct hif_msg *input, struct hif_sl_msg *output)
{
	return -EIO;
}

static inline int wfx_sl_check_pubkey(struct wfx_dev *wdev, uint8_t *ncp_pubkey, uint8_t *ncp_pubmac)
{
	return -EIO;
}

static inline int wfx_sl_init(struct wfx_dev *wdev)
{
	return -EIO;
}

static inline void wfx_sl_deinit(struct wfx_dev *wdev)
{
}


#endif
