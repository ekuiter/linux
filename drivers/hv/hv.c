/*
 * Copyright (c) 2009, Microsoft Corporation.
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
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/hyperv.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/clockchips.h>
#include <asm/hyperv.h>
#include <asm/mshyperv.h>
#include "hyperv_vmbus.h"

/* The one and only */
struct hv_context hv_context = {
	.synic_initialized	= false,
};

#define HV_TIMER_FREQUENCY (10 * 1000 * 1000) /* 100ns period */
#define HV_MAX_MAX_DELTA_TICKS 0xffffffff
#define HV_MIN_DELTA_TICKS 1

/*
 * query_hypervisor_info - Get version info of the windows hypervisor
 */
unsigned int host_info_eax;
unsigned int host_info_ebx;
unsigned int host_info_ecx;
unsigned int host_info_edx;

static int query_hypervisor_info(void)
{
	unsigned int eax;
	unsigned int ebx;
	unsigned int ecx;
	unsigned int edx;
	unsigned int max_leaf;
	unsigned int op;

	/*
	* Its assumed that this is called after confirming that Viridian
	* is present. Query id and revision.
	*/
	eax = 0;
	ebx = 0;
	ecx = 0;
	edx = 0;
	op = HVCPUID_VENDOR_MAXFUNCTION;
	cpuid(op, &eax, &ebx, &ecx, &edx);

	max_leaf = eax;

	if (max_leaf >= HVCPUID_VERSION) {
		eax = 0;
		ebx = 0;
		ecx = 0;
		edx = 0;
		op = HVCPUID_VERSION;
		cpuid(op, &eax, &ebx, &ecx, &edx);
		host_info_eax = eax;
		host_info_ebx = ebx;
		host_info_ecx = ecx;
		host_info_edx = edx;
	}
	return max_leaf;
}

/*
 * hv_init - Main initialization routine.
 *
 * This routine must be called before any other routines in here are called
 */
int hv_init(void)
{
	int max_leaf;
	union hv_x64_msr_hypercall_contents hypercall_msr;

	memset(hv_context.synic_event_page, 0, sizeof(void *) * NR_CPUS);
	memset(hv_context.synic_message_page, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.post_msg_page, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.vp_index, 0,
	       sizeof(int) * NR_CPUS);
	memset(hv_context.event_dpc, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.msg_dpc, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.clk_evt, 0,
	       sizeof(void *) * NR_CPUS);

	max_leaf = query_hypervisor_info();


	/* See if the hypercall page is already set */
	hypercall_msr.as_uint64 = 0;
	rdmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr.as_uint64);

	if (!hypercall_msr.enable)
		return -ENOTSUPP;

	return 0;
}

/*
 * hv_cleanup - Cleanup routine.
 *
 * This routine is called normally during driver unloading or exiting.
 */
void hv_cleanup(bool crash)
{

}

/*
 * hv_post_message - Post a message using the hypervisor message IPC.
 *
 * This involves a hypercall.
 */
int hv_post_message(union hv_connection_id connection_id,
		  enum hv_message_type message_type,
		  void *payload, size_t payload_size)
{

	struct hv_input_post_message *aligned_msg;
	u64 status;

	if (payload_size > HV_MESSAGE_PAYLOAD_BYTE_COUNT)
		return -EMSGSIZE;

	aligned_msg = (struct hv_input_post_message *)
			hv_context.post_msg_page[get_cpu()];

	aligned_msg->connectionid = connection_id;
	aligned_msg->reserved = 0;
	aligned_msg->message_type = message_type;
	aligned_msg->payload_size = payload_size;
	memcpy((void *)aligned_msg->payload, payload, payload_size);

	status = hv_do_hypercall(HVCALL_POST_MESSAGE, aligned_msg, NULL);

	put_cpu();
	return status & 0xFFFF;
}

static int hv_ce_set_next_event(unsigned long delta,
				struct clock_event_device *evt)
{
	u64 current_tick;

	WARN_ON(!clockevent_state_oneshot(evt));

	rdmsrl(HV_X64_MSR_TIME_REF_COUNT, current_tick);
	current_tick += delta;
	wrmsrl(HV_X64_MSR_STIMER0_COUNT, current_tick);
	return 0;
}

static int hv_ce_shutdown(struct clock_event_device *evt)
{
	wrmsrl(HV_X64_MSR_STIMER0_COUNT, 0);
	wrmsrl(HV_X64_MSR_STIMER0_CONFIG, 0);

	return 0;
}

static int hv_ce_set_oneshot(struct clock_event_device *evt)
{
	union hv_timer_config timer_cfg;

	timer_cfg.enable = 1;
	timer_cfg.auto_enable = 1;
	timer_cfg.sintx = VMBUS_MESSAGE_SINT;
	wrmsrl(HV_X64_MSR_STIMER0_CONFIG, timer_cfg.as_uint64);

	return 0;
}

static void hv_init_clockevent_device(struct clock_event_device *dev, int cpu)
{
	dev->name = "Hyper-V clockevent";
	dev->features = CLOCK_EVT_FEAT_ONESHOT;
	dev->cpumask = cpumask_of(cpu);
	dev->rating = 1000;
	/*
	 * Avoid settint dev->owner = THIS_MODULE deliberately as doing so will
	 * result in clockevents_config_and_register() taking additional
	 * references to the hv_vmbus module making it impossible to unload.
	 */

	dev->set_state_shutdown = hv_ce_shutdown;
	dev->set_state_oneshot = hv_ce_set_oneshot;
	dev->set_next_event = hv_ce_set_next_event;
}


int hv_synic_alloc(void)
{
	size_t size = sizeof(struct tasklet_struct);
	size_t ced_size = sizeof(struct clock_event_device);
	int cpu;

	hv_context.hv_numa_map = kzalloc(sizeof(struct cpumask) * nr_node_ids,
					 GFP_ATOMIC);
	if (hv_context.hv_numa_map == NULL) {
		pr_err("Unable to allocate NUMA map\n");
		goto err;
	}

	for_each_present_cpu(cpu) {
		hv_context.event_dpc[cpu] = kmalloc(size, GFP_ATOMIC);
		if (hv_context.event_dpc[cpu] == NULL) {
			pr_err("Unable to allocate event dpc\n");
			goto err;
		}
		tasklet_init(hv_context.event_dpc[cpu], vmbus_on_event, cpu);

		hv_context.msg_dpc[cpu] = kmalloc(size, GFP_ATOMIC);
		if (hv_context.msg_dpc[cpu] == NULL) {
			pr_err("Unable to allocate event dpc\n");
			goto err;
		}
		tasklet_init(hv_context.msg_dpc[cpu], vmbus_on_msg_dpc, cpu);

		hv_context.clk_evt[cpu] = kzalloc(ced_size, GFP_ATOMIC);
		if (hv_context.clk_evt[cpu] == NULL) {
			pr_err("Unable to allocate clock event device\n");
			goto err;
		}

		hv_init_clockevent_device(hv_context.clk_evt[cpu], cpu);

		hv_context.synic_message_page[cpu] =
			(void *)get_zeroed_page(GFP_ATOMIC);

		if (hv_context.synic_message_page[cpu] == NULL) {
			pr_err("Unable to allocate SYNIC message page\n");
			goto err;
		}

		hv_context.synic_event_page[cpu] =
			(void *)get_zeroed_page(GFP_ATOMIC);

		if (hv_context.synic_event_page[cpu] == NULL) {
			pr_err("Unable to allocate SYNIC event page\n");
			goto err;
		}

		hv_context.post_msg_page[cpu] =
			(void *)get_zeroed_page(GFP_ATOMIC);

		if (hv_context.post_msg_page[cpu] == NULL) {
			pr_err("Unable to allocate post msg page\n");
			goto err;
		}

		INIT_LIST_HEAD(&hv_context.percpu_list[cpu]);
	}

	return 0;
err:
	return -ENOMEM;
}

static void hv_synic_free_cpu(int cpu)
{
	kfree(hv_context.event_dpc[cpu]);
	kfree(hv_context.msg_dpc[cpu]);
	kfree(hv_context.clk_evt[cpu]);
	if (hv_context.synic_event_page[cpu])
		free_page((unsigned long)hv_context.synic_event_page[cpu]);
	if (hv_context.synic_message_page[cpu])
		free_page((unsigned long)hv_context.synic_message_page[cpu]);
	if (hv_context.post_msg_page[cpu])
		free_page((unsigned long)hv_context.post_msg_page[cpu]);
}

void hv_synic_free(void)
{
	int cpu;

	kfree(hv_context.hv_numa_map);
	for_each_present_cpu(cpu)
		hv_synic_free_cpu(cpu);
}

/*
 * hv_synic_init - Initialize the Synthethic Interrupt Controller.
 *
 * If it is already initialized by another entity (ie x2v shim), we need to
 * retrieve the initialized message and event pages.  Otherwise, we create and
 * initialize the message and event pages.
 */
int hv_synic_init(unsigned int cpu)
{
	u64 version;
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;
	union hv_synic_sint shared_sint;
	union hv_synic_scontrol sctrl;
	u64 vp_index;

	/* Check the version */
	rdmsrl(HV_X64_MSR_SVERSION, version);

	/* Setup the Synic's message page */
	rdmsrl(HV_X64_MSR_SIMP, simp.as_uint64);
	simp.simp_enabled = 1;
	simp.base_simp_gpa = virt_to_phys(hv_context.synic_message_page[cpu])
		>> PAGE_SHIFT;

	wrmsrl(HV_X64_MSR_SIMP, simp.as_uint64);

	/* Setup the Synic's event page */
	rdmsrl(HV_X64_MSR_SIEFP, siefp.as_uint64);
	siefp.siefp_enabled = 1;
	siefp.base_siefp_gpa = virt_to_phys(hv_context.synic_event_page[cpu])
		>> PAGE_SHIFT;

	wrmsrl(HV_X64_MSR_SIEFP, siefp.as_uint64);

	/* Setup the shared SINT. */
	rdmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);

	shared_sint.as_uint64 = 0;
	shared_sint.vector = HYPERVISOR_CALLBACK_VECTOR;
	shared_sint.masked = false;
	shared_sint.auto_eoi = true;

	wrmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);

	/* Enable the global synic bit */
	rdmsrl(HV_X64_MSR_SCONTROL, sctrl.as_uint64);
	sctrl.enable = 1;

	wrmsrl(HV_X64_MSR_SCONTROL, sctrl.as_uint64);

	hv_context.synic_initialized = true;

	/*
	 * Setup the mapping between Hyper-V's notion
	 * of cpuid and Linux' notion of cpuid.
	 * This array will be indexed using Linux cpuid.
	 */
	rdmsrl(HV_X64_MSR_VP_INDEX, vp_index);
	hv_context.vp_index[cpu] = (u32)vp_index;

	/*
	 * Register the per-cpu clockevent source.
	 */
	if (ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE)
		clockevents_config_and_register(hv_context.clk_evt[cpu],
						HV_TIMER_FREQUENCY,
						HV_MIN_DELTA_TICKS,
						HV_MAX_MAX_DELTA_TICKS);
	return 0;
}

/*
 * hv_synic_clockevents_cleanup - Cleanup clockevent devices
 */
void hv_synic_clockevents_cleanup(void)
{
	int cpu;

	if (!(ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE))
		return;

	for_each_present_cpu(cpu)
		clockevents_unbind_device(hv_context.clk_evt[cpu], cpu);
}

/*
 * hv_synic_cleanup - Cleanup routine for hv_synic_init().
 */
int hv_synic_cleanup(unsigned int cpu)
{
	union hv_synic_sint shared_sint;
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;
	union hv_synic_scontrol sctrl;
	struct vmbus_channel *channel, *sc;
	bool channel_found = false;
	unsigned long flags;

	if (!hv_context.synic_initialized)
		return -EFAULT;

	/*
	 * Search for channels which are bound to the CPU we're about to
	 * cleanup. In case we find one and vmbus is still connected we need to
	 * fail, this will effectively prevent CPU offlining. There is no way
	 * we can re-bind channels to different CPUs for now.
	 */
	mutex_lock(&vmbus_connection.channel_mutex);
	list_for_each_entry(channel, &vmbus_connection.chn_list, listentry) {
		if (channel->target_cpu == cpu) {
			channel_found = true;
			break;
		}
		spin_lock_irqsave(&channel->lock, flags);
		list_for_each_entry(sc, &channel->sc_list, sc_list) {
			if (sc->target_cpu == cpu) {
				channel_found = true;
				break;
			}
		}
		spin_unlock_irqrestore(&channel->lock, flags);
		if (channel_found)
			break;
	}
	mutex_unlock(&vmbus_connection.channel_mutex);

	if (channel_found && vmbus_connection.conn_state == CONNECTED)
		return -EBUSY;

	/* Turn off clockevent device */
	if (ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE) {
		clockevents_unbind_device(hv_context.clk_evt[cpu], cpu);
		hv_ce_shutdown(hv_context.clk_evt[cpu]);
	}

	rdmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);

	shared_sint.masked = 1;

	/* Need to correctly cleanup in the case of SMP!!! */
	/* Disable the interrupt */
	wrmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);

	rdmsrl(HV_X64_MSR_SIMP, simp.as_uint64);
	simp.simp_enabled = 0;
	simp.base_simp_gpa = 0;

	wrmsrl(HV_X64_MSR_SIMP, simp.as_uint64);

	rdmsrl(HV_X64_MSR_SIEFP, siefp.as_uint64);
	siefp.siefp_enabled = 0;
	siefp.base_siefp_gpa = 0;

	wrmsrl(HV_X64_MSR_SIEFP, siefp.as_uint64);

	/* Disable the global synic bit */
	rdmsrl(HV_X64_MSR_SCONTROL, sctrl.as_uint64);
	sctrl.enable = 0;
	wrmsrl(HV_X64_MSR_SCONTROL, sctrl.as_uint64);

	return 0;
}
