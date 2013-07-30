/*
 * arch/arm/common/bL_switcher.c -- big.LITTLE cluster switcher core driver
 *
 * Created by:	Nicolas Pitre, March 2012
 * Copyright:	(C) 2012-2013  Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/clockchips.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/irqchip/arm-gic.h>

#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/mcpm.h>
#include <asm/bL_switcher.h>


/*
 * Use our own MPIDR accessors as the generic ones in asm/cputype.h have
 * __attribute_const__ and we don't want the compiler to assume any
 * constness here as the value _does_ change along some code paths.
 */

static int read_mpidr(void)
{
	unsigned int id;
	asm volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r" (id));
	return id & MPIDR_HWID_BITMASK;
}

/*
 * bL switcher core code.
 */

static void bL_do_switch(void *_unused)
{
	unsigned mpidr, cpuid, clusterid, ob_cluster, ib_cluster;

	/*
	 * We now have a piece of stack borrowed from the init task's.
	 * Let's also switch to init_mm right away to match it.
	 */
	cpu_switch_mm(init_mm.pgd, &init_mm);

	pr_debug("%s\n", __func__);

	mpidr = read_mpidr();
	cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	ob_cluster = clusterid;
	ib_cluster = clusterid ^ 1;

	/*
	 * Our state has been saved at this point.  Let's release our
	 * inbound CPU.
	 */
	mcpm_set_entry_vector(cpuid, ib_cluster, cpu_resume);
	sev();

	/*
	 * From this point, we must assume that our counterpart CPU might
	 * have taken over in its parallel world already, as if execution
	 * just returned from cpu_suspend().  It is therefore important to
	 * be very careful not to make any change the other guy is not
	 * expecting.  This is why we need stack isolation.
	 *
	 * Fancy under cover tasks could be performed here.  For now
	 * we have none.
	 */

	/* Let's put ourself down. */
	mcpm_cpu_power_down();

	/* should never get here */
	BUG();
}

/*
 * Stack isolation.  To ensure 'current' remains valid, we just borrow
 * a slice of the init/idle task which should be fairly lightly used.
 * The borrowed area starts just above the thread_info structure located
 * at the very bottom of the stack, aligned to a cache line.
 */
#define STACK_SIZE 256
extern void call_with_stack(void (*fn)(void *), void *arg, void *sp);
static int bL_switchpoint(unsigned long _arg)
{
	unsigned int mpidr = read_mpidr();
	unsigned int cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	unsigned int clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int cpu_index = cpuid + clusterid * MAX_CPUS_PER_CLUSTER;
	void *stack = &init_thread_info + 1;
	stack = PTR_ALIGN(stack, L1_CACHE_BYTES);
	stack += cpu_index * STACK_SIZE + STACK_SIZE;
	call_with_stack(bL_do_switch, (void *)_arg, stack);
	BUG();
}

/*
 * Generic switcher interface
 */

/*
 * bL_switch_to - Switch to a specific cluster for the current CPU
 * @new_cluster_id: the ID of the cluster to switch to.
 *
 * This function must be called on the CPU to be switched.
 * Returns 0 on success, else a negative status code.
 */
static int bL_switch_to(unsigned int new_cluster_id)
{
	unsigned int mpidr, cpuid, clusterid, ob_cluster, ib_cluster, this_cpu;
	struct tick_device *tdev;
	enum clock_event_mode tdev_mode;
	int ret;

	mpidr = read_mpidr();
	cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	ob_cluster = clusterid;
	ib_cluster = clusterid ^ 1;

	if (new_cluster_id == clusterid)
		return 0;

	pr_debug("before switch: CPU %d in cluster %d\n", cpuid, clusterid);

	/* Close the gate for our entry vectors */
	mcpm_set_entry_vector(cpuid, ob_cluster, NULL);
	mcpm_set_entry_vector(cpuid, ib_cluster, NULL);

	/*
	 * Let's wake up the inbound CPU now in case it requires some delay
	 * to come online, but leave it gated in our entry vector code.
	 */
	ret = mcpm_cpu_power_up(cpuid, ib_cluster);
	if (ret) {
		pr_err("%s: mcpm_cpu_power_up() returned %d\n", __func__, ret);
		return ret;
	}

	/*
	 * From this point we are entering the switch critical zone
	 * and can't take any interrupts anymore.
	 */
	local_irq_disable();
	local_fiq_disable();

	this_cpu = smp_processor_id();

	/* redirect GIC's SGIs to our counterpart */
	gic_migrate_target(cpuid + ib_cluster*4);

	/*
	 * Raise a SGI on the inbound CPU to make sure it doesn't stall
	 * in a possible WFI, such as in mcpm_power_down().
	 */
	arch_send_wakeup_ipi_mask(cpumask_of(this_cpu));

	tdev = tick_get_device(this_cpu);
	if (tdev && !cpumask_equal(tdev->evtdev->cpumask, cpumask_of(this_cpu)))
		tdev = NULL;
	if (tdev) {
		tdev_mode = tdev->evtdev->mode;
		clockevents_set_mode(tdev->evtdev, CLOCK_EVT_MODE_SHUTDOWN);
	}

	ret = cpu_pm_enter();

	/* we can not tolerate errors at this point */
	if (ret)
		panic("%s: cpu_pm_enter() returned %d\n", __func__, ret);

	/* Flip the cluster in the CPU logical map for this CPU. */
	cpu_logical_map(this_cpu) ^= (1 << 8);

	/* Let's do the actual CPU switch. */
	ret = cpu_suspend(0, bL_switchpoint);
	if (ret > 0)
		panic("%s: cpu_suspend() returned %d\n", __func__, ret);

	/* We are executing on the inbound CPU at this point */
	mpidr = read_mpidr();
	cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	pr_debug("after switch: CPU %d in cluster %d\n", cpuid, clusterid);
	BUG_ON(clusterid != ib_cluster);

	mcpm_cpu_powered_up();

	ret = cpu_pm_exit();

	if (tdev) {
		clockevents_set_mode(tdev->evtdev, tdev_mode);
		clockevents_program_event(tdev->evtdev,
					  tdev->evtdev->next_event, 1);
	}

	local_fiq_enable();
	local_irq_enable();

	if (ret)
		pr_err("%s exiting with error %d\n", __func__, ret);
	return ret;
}

struct bL_thread {
	struct task_struct *task;
	wait_queue_head_t wq;
	int wanted_cluster;
};

static struct bL_thread bL_threads[NR_CPUS];

static int bL_switcher_thread(void *arg)
{
	struct bL_thread *t = arg;
	struct sched_param param = { .sched_priority = 1 };
	int cluster;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &param);

	do {
		if (signal_pending(current))
			flush_signals(current);
		wait_event_interruptible(t->wq,
				t->wanted_cluster != -1 ||
				kthread_should_stop());
		cluster = xchg(&t->wanted_cluster, -1);
		if (cluster != -1)
			bL_switch_to(cluster);
	} while (!kthread_should_stop());

	return 0;
}

static struct task_struct * __init bL_switcher_thread_create(int cpu, void *arg)
{
	struct task_struct *task;

	task = kthread_create_on_node(bL_switcher_thread, arg,
				      cpu_to_node(cpu), "kswitcher_%d", cpu);
	if (!IS_ERR(task)) {
		kthread_bind(task, cpu);
		wake_up_process(task);
	} else
		pr_err("%s failed for CPU %d\n", __func__, cpu);
	return task;
}

/*
 * bL_switch_request - Switch to a specific cluster for the given CPU
 *
 * @cpu: the CPU to switch
 * @new_cluster_id: the ID of the cluster to switch to.
 *
 * This function causes a cluster switch on the given CPU by waking up
 * the appropriate switcher thread.  This function may or may not return
 * before the switch has occurred.
 */
int bL_switch_request(unsigned int cpu, unsigned int new_cluster_id)
{
	struct bL_thread *t;

	if (cpu >= ARRAY_SIZE(bL_threads)) {
		pr_err("%s: cpu %d out of bounds\n", __func__, cpu);
		return -EINVAL;
	}

	t = &bL_threads[cpu];
	if (IS_ERR(t->task))
		return PTR_ERR(t->task);
	if (!t->task)
		return -ESRCH;

	t->wanted_cluster = new_cluster_id;
	wake_up(&t->wq);
	return 0;
}
EXPORT_SYMBOL_GPL(bL_switch_request);

static int __init bL_switcher_init(void)
{
	int cpu;

	pr_info("big.LITTLE switcher initializing\n");

	for_each_online_cpu(cpu) {
		struct bL_thread *t = &bL_threads[cpu];
		init_waitqueue_head(&t->wq);
		t->wanted_cluster = -1;
		t->task = bL_switcher_thread_create(cpu, t);
	}

	pr_info("big.LITTLE switcher initialized\n");
	return 0;
}

late_initcall(bL_switcher_init);
