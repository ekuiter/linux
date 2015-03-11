/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __QCOM_SCM_H
#define __QCOM_SCM_H

#define QCOM_SCM_FLAG_COLDBOOT_CPU1		0x01
#define QCOM_SCM_FLAG_COLDBOOT_CPU2		0x08
#define QCOM_SCM_FLAG_COLDBOOT_CPU3		0x20
#define QCOM_SCM_FLAG_WARMBOOT_CPU0		0x04
#define QCOM_SCM_FLAG_WARMBOOT_CPU1		0x02
#define QCOM_SCM_FLAG_WARMBOOT_CPU2		0x10
#define QCOM_SCM_FLAG_WARMBOOT_CPU3		0x40

extern int qcom_scm_set_boot_addr(u32 addr, int flags);

#define QCOM_SCM_VERSION(major, minor) (((major) << 16) | ((minor) & 0xFF))

extern u32 qcom_scm_get_version(void);

#endif
