/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2021-2022 Intel Corporation. All rights reserved.
 *
 * Authors: Cezary Rojewski <cezary.rojewski@intel.com>
 *          Amadeusz Slawinski <amadeuszx.slawinski@linux.intel.com>
 */

#ifndef __SOUND_SOC_INTEL_AVS_H
#define __SOUND_SOC_INTEL_AVS_H

#include <linux/device.h>
#include <sound/hda_codec.h>
#include "messages.h"

struct avs_dev;

/*
 * struct avs_dsp_ops - Platform-specific DSP operations
 *
 * @power: Power on or off DSP cores
 * @reset: Enter or exit reset state on DSP cores
 * @stall: Stall or run DSP cores
 * @irq_handler: Top half of IPC servicing
 * @irq_thread: Bottom half of IPC servicing
 * @int_control: Enable or disable IPC interrupts
 */
struct avs_dsp_ops {
	int (* const power)(struct avs_dev *, u32, bool);
	int (* const reset)(struct avs_dev *, u32, bool);
	int (* const stall)(struct avs_dev *, u32, bool);
	irqreturn_t (* const irq_handler)(int, void *);
	irqreturn_t (* const irq_thread)(int, void *);
	void (* const int_control)(struct avs_dev *, bool);
};

#define avs_dsp_op(adev, op, ...) \
	((adev)->spec->dsp_ops->op(adev, ## __VA_ARGS__))

#define avs_platattr_test(adev, attr) \
	((adev)->spec->attributes & AVS_PLATATTR_##attr)

/* Platform specific descriptor */
struct avs_spec {
	const char *name;

	const struct avs_dsp_ops *const dsp_ops;

	const u32 core_init_mask;	/* used during DSP boot */
	const u64 attributes;		/* bitmask of AVS_PLATATTR_* */
	const u32 sram_base_offset;
	const u32 sram_window_size;
	const u32 rom_status;
};

/*
 * struct avs_dev - Intel HD-Audio driver data
 *
 * @dev: PCI device
 * @dsp_ba: DSP bar address
 * @spec: platform-specific descriptor
 */
struct avs_dev {
	struct hda_bus base;
	struct device *dev;

	void __iomem *dsp_ba;
	const struct avs_spec *spec;
	struct avs_ipc *ipc;

	struct completion fw_ready;
};

/* from hda_bus to avs_dev */
#define hda_to_avs(hda) container_of(hda, struct avs_dev, base)
/* from hdac_bus to avs_dev */
#define hdac_to_avs(hdac) hda_to_avs(to_hda_bus(hdac))
/* from device to avs_dev */
#define to_avs_dev(dev) \
({ \
	struct hdac_bus *__bus = dev_get_drvdata(dev); \
	hdac_to_avs(__bus); \
})

int avs_dsp_core_power(struct avs_dev *adev, u32 core_mask, bool power);
int avs_dsp_core_reset(struct avs_dev *adev, u32 core_mask, bool reset);
int avs_dsp_core_stall(struct avs_dev *adev, u32 core_mask, bool stall);
int avs_dsp_core_enable(struct avs_dev *adev, u32 core_mask);
int avs_dsp_core_disable(struct avs_dev *adev, u32 core_mask);

/* Inter Process Communication */

struct avs_ipc_msg {
	union {
		u64 header;
		union avs_global_msg glb;
		union avs_reply_msg rsp;
	};
	void *data;
	size_t size;
};

/*
 * struct avs_ipc - DSP IPC context
 *
 * @dev: PCI device
 * @rx: Reply message cache
 * @default_timeout_ms: default message timeout in MS
 * @ready: whether firmware is ready and communication is open
 * @rx_completed: whether RX for previously sent TX has been received
 * @rx_lock: for serializing manipulation of rx_* fields
 * @msg_lock: for synchronizing request handling
 * @done_completion: DONE-part of IPC i.e. ROM and ACKs from FW
 * @busy_completion: BUSY-part of IPC i.e. receiving responses from FW
 */
struct avs_ipc {
	struct device *dev;

	struct avs_ipc_msg rx;
	u32 default_timeout_ms;
	bool ready;

	bool rx_completed;
	spinlock_t rx_lock;
	struct mutex msg_mutex;
	struct completion done_completion;
	struct completion busy_completion;
};

#define AVS_EIPC	EREMOTEIO
/*
 * IPC handlers may return positive value (firmware error code) what denotes
 * successful HOST <-> DSP communication yet failure to process specific request.
 *
 * Below macro converts returned value to linux kernel error code.
 * All IPC callers MUST use it as soon as firmware error code is consumed.
 */
#define AVS_IPC_RET(ret) \
	(((ret) <= 0) ? (ret) : -AVS_EIPC)

static inline void avs_ipc_err(struct avs_dev *adev, struct avs_ipc_msg *tx,
			       const char *name, int error)
{
	/*
	 * If IPC channel is blocked e.g.: due to ongoing recovery,
	 * -EPERM error code is expected and thus it's not an actual error.
	 */
	if (error == -EPERM)
		dev_dbg(adev->dev, "%s 0x%08x 0x%08x failed: %d\n", name,
			tx->glb.primary, tx->glb.ext.val, error);
	else
		dev_err(adev->dev, "%s 0x%08x 0x%08x failed: %d\n", name,
			tx->glb.primary, tx->glb.ext.val, error);
}

irqreturn_t avs_dsp_irq_handler(int irq, void *dev_id);
irqreturn_t avs_dsp_irq_thread(int irq, void *dev_id);
void avs_dsp_process_response(struct avs_dev *adev, u64 header);
int avs_dsp_send_msg_timeout(struct avs_dev *adev,
			     struct avs_ipc_msg *request,
			     struct avs_ipc_msg *reply, int timeout);
int avs_dsp_send_msg(struct avs_dev *adev,
		     struct avs_ipc_msg *request, struct avs_ipc_msg *reply);
int avs_dsp_send_rom_msg_timeout(struct avs_dev *adev,
				 struct avs_ipc_msg *request, int timeout);
int avs_dsp_send_rom_msg(struct avs_dev *adev, struct avs_ipc_msg *request);
void avs_dsp_interrupt_control(struct avs_dev *adev, bool enable);
int avs_ipc_init(struct avs_ipc *ipc, struct device *dev);
void avs_ipc_block(struct avs_ipc *ipc);

#endif /* __SOUND_SOC_INTEL_AVS_H */
