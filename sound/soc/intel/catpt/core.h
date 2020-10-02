/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Cezary Rojewski <cezary.rojewski@intel.com>
 */

#ifndef __SND_SOC_INTEL_CATPT_CORE_H
#define __SND_SOC_INTEL_CATPT_CORE_H

#include <linux/dma/dw.h>
#include <linux/irqreturn.h>
#include "messages.h"
#include "registers.h"

struct catpt_dev;

void catpt_sram_init(struct resource *sram, u32 start, u32 size);
void catpt_sram_free(struct resource *sram);
struct resource *
catpt_request_region(struct resource *root, resource_size_t size);

static inline bool catpt_resource_overlapping(struct resource *r1,
					      struct resource *r2,
					      struct resource *ret)
{
	if (!resource_overlaps(r1, r2))
		return false;
	ret->start = max(r1->start, r2->start);
	ret->end = min(r1->end, r2->end);
	return true;
}

struct catpt_ipc_msg {
	union {
		u32 header;
		union catpt_global_msg rsp;
	};
	void *data;
	size_t size;
};

struct catpt_ipc {
	struct device *dev;

	struct catpt_ipc_msg rx;
	struct catpt_fw_ready config;
	u32 default_timeout;
	bool ready;

	spinlock_t lock;
	struct mutex mutex;
	struct completion done_completion;
	struct completion busy_completion;
};

void catpt_ipc_init(struct catpt_ipc *ipc, struct device *dev);

struct catpt_module_type {
	bool loaded;
	u32 entry_point;
	u32 persistent_size;
	u32 scratch_size;
	/* DRAM, initial module state */
	u32 state_offset;
	u32 state_size;

	struct list_head node;
};

struct catpt_spec {
	struct snd_soc_acpi_mach *machines;
	u8 core_id;
	u32 host_dram_offset;
	u32 host_iram_offset;
	u32 host_shim_offset;
	u32 host_dma_offset[CATPT_DMA_COUNT];
	u32 host_ssp_offset[CATPT_SSP_COUNT];
	u32 dram_mask;
	u32 iram_mask;
	void (*pll_shutdown)(struct catpt_dev *cdev, bool enable);
	int (*power_up)(struct catpt_dev *cdev);
	int (*power_down)(struct catpt_dev *cdev);
};

struct catpt_dev {
	struct device *dev;
	struct dw_dma_chip *dmac;
	struct catpt_ipc ipc;

	void __iomem *pci_ba;
	void __iomem *lpe_ba;
	u32 lpe_base;
	int irq;

	const struct catpt_spec *spec;
	struct completion fw_ready;

	struct resource dram;
	struct resource iram;
	struct resource *scratch;
};

int catpt_dmac_probe(struct catpt_dev *cdev);
void catpt_dmac_remove(struct catpt_dev *cdev);
struct dma_chan *catpt_dma_request_config_chan(struct catpt_dev *cdev);
int catpt_dma_memcpy_todsp(struct catpt_dev *cdev, struct dma_chan *chan,
			   dma_addr_t dst_addr, dma_addr_t src_addr,
			   size_t size);
int catpt_dma_memcpy_fromdsp(struct catpt_dev *cdev, struct dma_chan *chan,
			     dma_addr_t dst_addr, dma_addr_t src_addr,
			     size_t size);

irqreturn_t catpt_dsp_irq_handler(int irq, void *dev_id);
irqreturn_t catpt_dsp_irq_thread(int irq, void *dev_id);

/*
 * IPC handlers may return positive values which denote successful
 * HOST <-> DSP communication yet failure to process specific request.
 * Use below macro to convert returned non-zero values appropriately
 */
#define CATPT_IPC_ERROR(err) (((err) < 0) ? (err) : -EREMOTEIO)

int catpt_dsp_send_msg_timeout(struct catpt_dev *cdev,
			       struct catpt_ipc_msg request,
			       struct catpt_ipc_msg *reply, int timeout);
int catpt_dsp_send_msg(struct catpt_dev *cdev, struct catpt_ipc_msg request,
		       struct catpt_ipc_msg *reply);

#endif
