/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 ******************************************************************************/
#ifndef __OSDEP_SERVICE_H_
#define __OSDEP_SERVICE_H_

#define _FAIL		0
#define _SUCCESS	1
#define RTW_RX_HANDLED 2

#include <linux/version.h>
#include <linux/spinlock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <asm/uaccess.h>
#include <asm/byteorder.h>
#include <asm/atomic.h>
#include <asm/io.h>
#include <linux/semaphore.h>
#include <linux/sem.h>
#include <linux/sched.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>	/*  Necessary because we use the proc fs */
#include <linux/interrupt.h>	/*  for struct tasklet_struct */
#include <linux/ip.h>
#include <linux/kthread.h>


/*	#include <linux/ieee80211.h> */
#include <net/ieee80211_radiotap.h>
#include <net/cfg80211.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>

struct rtw_adapter;
struct c2h_evt_hdr;

typedef s32 (*c2h_id_filter)(u8 id);

struct rtw_queue {
	struct	list_head	queue;
	spinlock_t		lock;
};

static inline struct list_head *get_list_head(struct rtw_queue *queue)
{
	return (&queue->queue);
}

static inline int rtw_netif_queue_stopped(struct net_device *pnetdev)
{
	return (netif_tx_queue_stopped(netdev_get_tx_queue(pnetdev, 0)) &&
		netif_tx_queue_stopped(netdev_get_tx_queue(pnetdev, 1)) &&
		netif_tx_queue_stopped(netdev_get_tx_queue(pnetdev, 2)) &&
		netif_tx_queue_stopped(netdev_get_tx_queue(pnetdev, 3)) );
}

#ifndef BIT
#define BIT(x)	( 1 << (x))
#endif
static inline u32 CHKBIT(u32 x)
{
	WARN_ON(x >= 32);
	if (x >= 32)
		return 0;
	return BIT(x);
}

#define BIT0	0x00000001
#define BIT1	0x00000002
#define BIT2	0x00000004
#define BIT3	0x00000008
#define BIT4	0x00000010
#define BIT5	0x00000020
#define BIT6	0x00000040
#define BIT7	0x00000080
#define BIT8	0x00000100
#define BIT9	0x00000200
#define BIT10	0x00000400
#define BIT11	0x00000800
#define BIT12	0x00001000
#define BIT13	0x00002000
#define BIT14	0x00004000
#define BIT15	0x00008000
#define BIT16	0x00010000
#define BIT17	0x00020000
#define BIT18	0x00040000
#define BIT19	0x00080000
#define BIT20	0x00100000
#define BIT21	0x00200000
#define BIT22	0x00400000
#define BIT23	0x00800000
#define BIT24	0x01000000
#define BIT25	0x02000000
#define BIT26	0x04000000
#define BIT27	0x08000000
#define BIT28	0x10000000
#define BIT29	0x20000000
#define BIT30	0x40000000
#define BIT31	0x80000000
#define BIT32	0x0100000000
#define BIT33	0x0200000000
#define BIT34	0x0400000000
#define BIT35	0x0800000000
#define BIT36	0x1000000000

extern unsigned char REALTEK_96B_IE23A[];
extern unsigned char MCS_rate_2R23A[16];
extern unsigned char WPA_TKIP_CIPHER23A[4];
extern unsigned char RSN_TKIP_CIPHER23A[4];

extern unsigned char	MCS_rate_2R23A[16];
extern unsigned char	MCS_rate_1R23A[16];

void	_rtw_init_queue23a(struct rtw_queue *pqueue);


#define ADPT_FMT "%s"
#define ADPT_ARG(adapter) adapter->pnetdev->name
#define FUNC_NDEV_FMT "%s(%s)"
#define FUNC_NDEV_ARG(ndev) __func__, ndev->name
#define FUNC_ADPT_FMT "%s(%s)"
#define FUNC_ADPT_ARG(adapter) __func__, adapter->pnetdev->name

/* Macros for handling unaligned memory accesses */

#define RTW_GET_BE24(a) ((((u32) (a)[0]) << 16) | (((u32) (a)[1]) << 8) | \
			 ((u32) (a)[2]))

s32 c2h_evt_hdl(struct rtw_adapter *adapter, struct c2h_evt_hdr *c2h_evt, c2h_id_filter filter);
u8 rtw_do_join23a(struct rtw_adapter *padapter);

#endif
