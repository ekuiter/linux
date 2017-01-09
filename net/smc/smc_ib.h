/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for IB environment
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <Ursula Braun@linux.vnet.ibm.com>
 */

#ifndef _SMC_IB_H
#define _SMC_IB_H

#include <rdma/ib_verbs.h>

#define SMC_MAX_PORTS			2	/* Max # of ports */
#define SMC_GID_SIZE			sizeof(union ib_gid)

struct smc_ib_devices {			/* list of smc ib devices definition */
	struct list_head	list;
	spinlock_t		lock;	/* protects list of smc ib devices */
};

extern struct smc_ib_devices	smc_ib_devices; /* list of smc ib devices */

struct smc_ib_device {				/* ib-device infos for smc */
	struct list_head	list;
	struct ib_device	*ibdev;
	struct ib_port_attr	pattr[SMC_MAX_PORTS];	/* ib dev. port attrs */
	char			mac[SMC_MAX_PORTS][6]; /* mac address per port*/
	union ib_gid		gid[SMC_MAX_PORTS]; /* gid per port */
	u8			initialized : 1; /* ib dev CQ, evthdl done */
};

int smc_ib_register_client(void) __init;
void smc_ib_unregister_client(void);
bool smc_ib_port_active(struct smc_ib_device *smcibdev, u8 ibport);
int smc_ib_remember_port_attr(struct smc_ib_device *smcibdev, u8 ibport);

#endif
