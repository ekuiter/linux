// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Advanced Micro Devices, Inc.
//
// Authors: Ajit Kumar Pandey <AjitKumar.Pandey@amd.com>

/*
 * Hardware interface for Audio DSP on Renoir platform
 */

#include <linux/platform_device.h>
#include <linux/module.h>

#include "../ops.h"
#include "acp.h"

/* AMD Renoir DSP ops */
const struct snd_sof_dsp_ops sof_renoir_ops = {
	/* probe and remove */
	.probe			= amd_sof_acp_probe,
	.remove			= amd_sof_acp_remove,

	/* Register IO */
	.write			= sof_io_write,
	.read			= sof_io_read,
};
EXPORT_SYMBOL(sof_renoir_ops);

MODULE_IMPORT_NS(SND_SOC_SOF_AMD_COMMON);
MODULE_DESCRIPTION("RENOIR SOF Driver");
MODULE_LICENSE("Dual BSD/GPL");
