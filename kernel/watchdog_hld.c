/*
 * Detect hard lockups on a system
 *
 * started by Don Zickus, Copyright (C) 2010 Red Hat, Inc.
 *
 * Note: Most of this code is borrowed heavily from the original softlockup
 * detector, so thanks to Ingo for the initial implementation.
 * Some chunks also taken from the old x86-specific nmi watchdog code, thanks
 * to those contributors as well.
 */

#define pr_fmt(fmt) "NMI watchdog: " fmt

#include <linux/nmi.h>
#include <linux/module.h>
#include <linux/sched/debug.h>

#include <asm/irq_regs.h>
#include <linux/perf_event.h>

static DEFINE_PER_CPU(bool, hard_watchdog_warn);
static DEFINE_PER_CPU(bool, watchdog_nmi_touch);
static DEFINE_PER_CPU(struct perf_event *, watchdog_ev);
static DEFINE_PER_CPU(struct perf_event *, dead_event);
static struct cpumask dead_events_mask;

static unsigned long hardlockup_allcpu_dumped;
static bool hardlockup_detector_disabled;

void arch_touch_nmi_watchdog(void)
{
	/*
	 * Using __raw here because some code paths have
	 * preemption enabled.  If preemption is enabled
	 * then interrupts should be enabled too, in which
	 * case we shouldn't have to worry about the watchdog
	 * going off.
	 */
	raw_cpu_write(watchdog_nmi_touch, true);
}
EXPORT_SYMBOL(arch_touch_nmi_watchdog);

#ifdef CONFIG_HARDLOCKUP_CHECK_TIMESTAMP
static DEFINE_PER_CPU(ktime_t, last_timestamp);
static DEFINE_PER_CPU(unsigned int, nmi_rearmed);
static ktime_t watchdog_hrtimer_sample_threshold __read_mostly;

void watchdog_update_hrtimer_threshold(u64 period)
{
	/*
	 * The hrtimer runs with a period of (watchdog_threshold * 2) / 5
	 *
	 * So it runs effectively with 2.5 times the rate of the NMI
	 * watchdog. That means the hrtimer should fire 2-3 times before
	 * the NMI watchdog expires. The NMI watchdog on x86 is based on
	 * unhalted CPU cycles, so if Turbo-Mode is enabled the CPU cycles
	 * might run way faster than expected and the NMI fires in a
	 * smaller period than the one deduced from the nominal CPU
	 * frequency. Depending on the Turbo-Mode factor this might be fast
	 * enough to get the NMI period smaller than the hrtimer watchdog
	 * period and trigger false positives.
	 *
	 * The sample threshold is used to check in the NMI handler whether
	 * the minimum time between two NMI samples has elapsed. That
	 * prevents false positives.
	 *
	 * Set this to 4/5 of the actual watchdog threshold period so the
	 * hrtimer is guaranteed to fire at least once within the real
	 * watchdog threshold.
	 */
	watchdog_hrtimer_sample_threshold = period * 2;
}

static bool watchdog_check_timestamp(void)
{
	ktime_t delta, now = ktime_get_mono_fast_ns();

	delta = now - __this_cpu_read(last_timestamp);
	if (delta < watchdog_hrtimer_sample_threshold) {
		/*
		 * If ktime is jiffies based, a stalled timer would prevent
		 * jiffies from being incremented and the filter would look
		 * at a stale timestamp and never trigger.
		 */
		if (__this_cpu_inc_return(nmi_rearmed) < 10)
			return false;
	}
	__this_cpu_write(nmi_rearmed, 0);
	__this_cpu_write(last_timestamp, now);
	return true;
}
#else
static inline bool watchdog_check_timestamp(void)
{
	return true;
}
#endif

static struct perf_event_attr wd_hw_attr = {
	.type		= PERF_TYPE_HARDWARE,
	.config		= PERF_COUNT_HW_CPU_CYCLES,
	.size		= sizeof(struct perf_event_attr),
	.pinned		= 1,
	.disabled	= 1,
};

/* Callback function for perf event subsystem */
static void watchdog_overflow_callback(struct perf_event *event,
				       struct perf_sample_data *data,
				       struct pt_regs *regs)
{
	/* Ensure the watchdog never gets throttled */
	event->hw.interrupts = 0;

	if (__this_cpu_read(watchdog_nmi_touch) == true) {
		__this_cpu_write(watchdog_nmi_touch, false);
		return;
	}

	if (!watchdog_check_timestamp())
		return;

	/* check for a hardlockup
	 * This is done by making sure our timer interrupt
	 * is incrementing.  The timer interrupt should have
	 * fired multiple times before we overflow'd.  If it hasn't
	 * then this is a good indication the cpu is stuck
	 */
	if (is_hardlockup()) {
		int this_cpu = smp_processor_id();

		/* only print hardlockups once */
		if (__this_cpu_read(hard_watchdog_warn) == true)
			return;

		pr_emerg("Watchdog detected hard LOCKUP on cpu %d", this_cpu);
		print_modules();
		print_irqtrace_events(current);
		if (regs)
			show_regs(regs);
		else
			dump_stack();

		/*
		 * Perform all-CPU dump only once to avoid multiple hardlockups
		 * generating interleaving traces
		 */
		if (sysctl_hardlockup_all_cpu_backtrace &&
				!test_and_set_bit(0, &hardlockup_allcpu_dumped))
			trigger_allbutself_cpu_backtrace();

		if (hardlockup_panic)
			nmi_panic(regs, "Hard LOCKUP");

		__this_cpu_write(hard_watchdog_warn, true);
		return;
	}

	__this_cpu_write(hard_watchdog_warn, false);
	return;
}

/*
 * People like the simple clean cpu node info on boot.
 * Reduce the watchdog noise by only printing messages
 * that are different from what cpu0 displayed.
 */
static unsigned long firstcpu_err;
static atomic_t watchdog_cpus;

int watchdog_nmi_enable(unsigned int cpu)
{
	struct perf_event_attr *wd_attr;
	struct perf_event *event = per_cpu(watchdog_ev, cpu);
	int firstcpu = 0;

	/* nothing to do if the hard lockup detector is disabled */
	if (!(watchdog_enabled & NMI_WATCHDOG_ENABLED))
		goto out;

	/* A failure disabled the hardlockup detector permanently */
	if (hardlockup_detector_disabled)
		return -ENODEV;

	/* is it already setup and enabled? */
	if (event && event->state > PERF_EVENT_STATE_OFF)
		goto out;

	/* it is setup but not enabled */
	if (event != NULL)
		goto out_enable;

	if (atomic_inc_return(&watchdog_cpus) == 1)
		firstcpu = 1;

	wd_attr = &wd_hw_attr;
	wd_attr->sample_period = hw_nmi_get_sample_period(watchdog_thresh);

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(wd_attr, cpu, NULL, watchdog_overflow_callback, NULL);

	/* save the first cpu's error for future comparision */
	if (firstcpu && IS_ERR(event))
		firstcpu_err = PTR_ERR(event);

	if (!IS_ERR(event)) {
		/* only print for the first cpu initialized */
		if (firstcpu || firstcpu_err)
			pr_info("enabled on all CPUs, permanently consumes one hw-PMU counter.\n");
		goto out_save;
	}

	/* skip displaying the same error again */
	if (!firstcpu && (PTR_ERR(event) == firstcpu_err))
		return PTR_ERR(event);

	/* vary the KERN level based on the returned errno */
	if (PTR_ERR(event) == -EOPNOTSUPP)
		pr_info("disabled (cpu%i): not supported (no LAPIC?)\n", cpu);
	else if (PTR_ERR(event) == -ENOENT)
		pr_warn("disabled (cpu%i): hardware events not enabled\n",
			 cpu);
	else
		pr_err("disabled (cpu%i): unable to create perf event: %ld\n",
			cpu, PTR_ERR(event));

	pr_info("Disabling hard lockup detector permanently\n");
	hardlockup_detector_disabled = true;

	return PTR_ERR(event);

	/* success path */
out_save:
	per_cpu(watchdog_ev, cpu) = event;
out_enable:
	perf_event_enable(per_cpu(watchdog_ev, cpu));
out:
	return 0;
}

/**
 * hardlockup_detector_perf_disable - Disable the local event
 */
void hardlockup_detector_perf_disable(void)
{
	struct perf_event *event = this_cpu_read(watchdog_ev);

	if (event) {
		perf_event_disable(event);
		this_cpu_write(watchdog_ev, NULL);
		this_cpu_write(dead_event, event);
		cpumask_set_cpu(smp_processor_id(), &dead_events_mask);

		/* watchdog_nmi_enable() expects this to be zero initially. */
		if (atomic_dec_and_test(&watchdog_cpus))
			firstcpu_err = 0;
	}
}

/**
 * hardlockup_detector_perf_cleanup - Cleanup disabled events and destroy them
 *
 * Called from lockup_detector_cleanup(). Serialized by the caller.
 */
void hardlockup_detector_perf_cleanup(void)
{
	int cpu;

	for_each_cpu(cpu, &dead_events_mask) {
		struct perf_event *event = per_cpu(dead_event, cpu);

		per_cpu(dead_event, cpu) = NULL;
		perf_event_release_kernel(event);
	}
	cpumask_clear(&dead_events_mask);
}

/**
 * hardlockup_detector_perf_stop - Globally stop watchdog events
 *
 * Special interface for x86 to handle the perf HT bug.
 */
void __init hardlockup_detector_perf_stop(void)
{
	int cpu;

	lockdep_assert_cpus_held();

	for_each_online_cpu(cpu) {
		struct perf_event *event = per_cpu(watchdog_ev, cpu);

		if (event)
			perf_event_disable(event);
	}
}

/**
 * hardlockup_detector_perf_restart - Globally restart watchdog events
 *
 * Special interface for x86 to handle the perf HT bug.
 */
void __init hardlockup_detector_perf_restart(void)
{
	int cpu;

	lockdep_assert_cpus_held();

	if (!(watchdog_enabled & NMI_WATCHDOG_ENABLED))
		return;

	for_each_online_cpu(cpu) {
		struct perf_event *event = per_cpu(watchdog_ev, cpu);

		if (event)
			perf_event_enable(event);
	}
}
