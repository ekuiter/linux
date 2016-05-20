/*
 * Copyright (C) 2015, 2016 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kvm.h>
#include <linux/kvm_host.h>

#include "vgic.h"

#define CREATE_TRACE_POINTS
#include "../trace.h"

#ifdef CONFIG_DEBUG_SPINLOCK
#define DEBUG_SPINLOCK_BUG_ON(p) BUG_ON(p)
#else
#define DEBUG_SPINLOCK_BUG_ON(p)
#endif

struct vgic_global __section(.hyp.text) kvm_vgic_global_state;

/*
 * Locking order is always:
 *   vgic_cpu->ap_list_lock
 *     vgic_irq->irq_lock
 *
 * (that is, always take the ap_list_lock before the struct vgic_irq lock).
 *
 * When taking more than one ap_list_lock at the same time, always take the
 * lowest numbered VCPU's ap_list_lock first, so:
 *   vcpuX->vcpu_id < vcpuY->vcpu_id:
 *     spin_lock(vcpuX->arch.vgic_cpu.ap_list_lock);
 *     spin_lock(vcpuY->arch.vgic_cpu.ap_list_lock);
 */

struct vgic_irq *vgic_get_irq(struct kvm *kvm, struct kvm_vcpu *vcpu,
			      u32 intid)
{
	/* SGIs and PPIs */
	if (intid <= VGIC_MAX_PRIVATE)
		return &vcpu->arch.vgic_cpu.private_irqs[intid];

	/* SPIs */
	if (intid <= VGIC_MAX_SPI)
		return &kvm->arch.vgic.spis[intid - VGIC_NR_PRIVATE_IRQS];

	/* LPIs are not yet covered */
	if (intid >= VGIC_MIN_LPI)
		return NULL;

	WARN(1, "Looking up struct vgic_irq for reserved INTID");
	return NULL;
}

/**
 * kvm_vgic_target_oracle - compute the target vcpu for an irq
 *
 * @irq:	The irq to route. Must be already locked.
 *
 * Based on the current state of the interrupt (enabled, pending,
 * active, vcpu and target_vcpu), compute the next vcpu this should be
 * given to. Return NULL if this shouldn't be injected at all.
 *
 * Requires the IRQ lock to be held.
 */
static struct kvm_vcpu *vgic_target_oracle(struct vgic_irq *irq)
{
	DEBUG_SPINLOCK_BUG_ON(!spin_is_locked(&irq->irq_lock));

	/* If the interrupt is active, it must stay on the current vcpu */
	if (irq->active)
		return irq->vcpu ? : irq->target_vcpu;

	/*
	 * If the IRQ is not active but enabled and pending, we should direct
	 * it to its configured target VCPU.
	 * If the distributor is disabled, pending interrupts shouldn't be
	 * forwarded.
	 */
	if (irq->enabled && irq->pending) {
		if (unlikely(irq->target_vcpu &&
			     !irq->target_vcpu->kvm->arch.vgic.enabled))
			return NULL;

		return irq->target_vcpu;
	}

	/* If neither active nor pending and enabled, then this IRQ should not
	 * be queued to any VCPU.
	 */
	return NULL;
}

/*
 * Only valid injection if changing level for level-triggered IRQs or for a
 * rising edge.
 */
static bool vgic_validate_injection(struct vgic_irq *irq, bool level)
{
	switch (irq->config) {
	case VGIC_CONFIG_LEVEL:
		return irq->line_level != level;
	case VGIC_CONFIG_EDGE:
		return level;
	}

	return false;
}

/*
 * Check whether an IRQ needs to (and can) be queued to a VCPU's ap list.
 * Do the queuing if necessary, taking the right locks in the right order.
 * Returns true when the IRQ was queued, false otherwise.
 *
 * Needs to be entered with the IRQ lock already held, but will return
 * with all locks dropped.
 */
bool vgic_queue_irq_unlock(struct kvm *kvm, struct vgic_irq *irq)
{
	struct kvm_vcpu *vcpu;

	DEBUG_SPINLOCK_BUG_ON(!spin_is_locked(&irq->irq_lock));

retry:
	vcpu = vgic_target_oracle(irq);
	if (irq->vcpu || !vcpu) {
		/*
		 * If this IRQ is already on a VCPU's ap_list, then it
		 * cannot be moved or modified and there is no more work for
		 * us to do.
		 *
		 * Otherwise, if the irq is not pending and enabled, it does
		 * not need to be inserted into an ap_list and there is also
		 * no more work for us to do.
		 */
		spin_unlock(&irq->irq_lock);
		return false;
	}

	/*
	 * We must unlock the irq lock to take the ap_list_lock where
	 * we are going to insert this new pending interrupt.
	 */
	spin_unlock(&irq->irq_lock);

	/* someone can do stuff here, which we re-check below */

	spin_lock(&vcpu->arch.vgic_cpu.ap_list_lock);
	spin_lock(&irq->irq_lock);

	/*
	 * Did something change behind our backs?
	 *
	 * There are two cases:
	 * 1) The irq lost its pending state or was disabled behind our
	 *    backs and/or it was queued to another VCPU's ap_list.
	 * 2) Someone changed the affinity on this irq behind our
	 *    backs and we are now holding the wrong ap_list_lock.
	 *
	 * In both cases, drop the locks and retry.
	 */

	if (unlikely(irq->vcpu || vcpu != vgic_target_oracle(irq))) {
		spin_unlock(&irq->irq_lock);
		spin_unlock(&vcpu->arch.vgic_cpu.ap_list_lock);

		spin_lock(&irq->irq_lock);
		goto retry;
	}

	list_add_tail(&irq->ap_list, &vcpu->arch.vgic_cpu.ap_list_head);
	irq->vcpu = vcpu;

	spin_unlock(&irq->irq_lock);
	spin_unlock(&vcpu->arch.vgic_cpu.ap_list_lock);

	kvm_vcpu_kick(vcpu);

	return true;
}

static int vgic_update_irq_pending(struct kvm *kvm, int cpuid,
				   unsigned int intid, bool level,
				   bool mapped_irq)
{
	struct kvm_vcpu *vcpu;
	struct vgic_irq *irq;
	int ret;

	trace_vgic_update_irq_pending(cpuid, intid, level);

	vcpu = kvm_get_vcpu(kvm, cpuid);
	if (!vcpu && intid < VGIC_NR_PRIVATE_IRQS)
		return -EINVAL;

	irq = vgic_get_irq(kvm, vcpu, intid);
	if (!irq)
		return -EINVAL;

	if (irq->hw != mapped_irq)
		return -EINVAL;

	spin_lock(&irq->irq_lock);

	if (!vgic_validate_injection(irq, level)) {
		/* Nothing to see here, move along... */
		spin_unlock(&irq->irq_lock);
		return 0;
	}

	if (irq->config == VGIC_CONFIG_LEVEL) {
		irq->line_level = level;
		irq->pending = level || irq->soft_pending;
	} else {
		irq->pending = true;
	}

	vgic_queue_irq_unlock(kvm, irq);

	return 0;
}

/**
 * kvm_vgic_inject_irq - Inject an IRQ from a device to the vgic
 * @kvm:     The VM structure pointer
 * @cpuid:   The CPU for PPIs
 * @intid:   The INTID to inject a new state to.
 * @level:   Edge-triggered:  true:  to trigger the interrupt
 *			      false: to ignore the call
 *	     Level-sensitive  true:  raise the input signal
 *			      false: lower the input signal
 *
 * The VGIC is not concerned with devices being active-LOW or active-HIGH for
 * level-sensitive interrupts.  You can think of the level parameter as 1
 * being HIGH and 0 being LOW and all devices being active-HIGH.
 */
int kvm_vgic_inject_irq(struct kvm *kvm, int cpuid, unsigned int intid,
			bool level)
{
	return vgic_update_irq_pending(kvm, cpuid, intid, level, false);
}
