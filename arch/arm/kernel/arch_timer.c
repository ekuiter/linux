/*
 *  linux/arch/arm/kernel/arch_timer.c
 *
 *  Copyright (C) 2011 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/io.h>

#include <asm/delay.h>
#include <asm/localtimer.h>
#include <asm/arch_timer.h>
#include <asm/sched_clock.h>

static u32 arch_timer_rate;

enum ppi_nr {
	PHYS_SECURE_PPI,
	PHYS_NONSECURE_PPI,
	VIRT_PPI,
	HYP_PPI,
	MAX_TIMER_PPI
};

static int arch_timer_ppi[MAX_TIMER_PPI];

static struct clock_event_device __percpu **arch_timer_evt;
static struct delay_timer arch_delay_timer;

static bool arch_timer_use_virtual = true;

/*
 * Architected system timer support.
 */

#define ARCH_TIMER_CTRL_ENABLE		(1 << 0)
#define ARCH_TIMER_CTRL_IT_MASK		(1 << 1)
#define ARCH_TIMER_CTRL_IT_STAT		(1 << 2)

#define ARCH_TIMER_REG_CTRL		0
#define ARCH_TIMER_REG_TVAL		1

#define ARCH_TIMER_PHYS_ACCESS		0
#define ARCH_TIMER_VIRT_ACCESS		1

/*
 * These register accessors are marked inline so the compiler can
 * nicely work out which register we want, and chuck away the rest of
 * the code. At least it does so with a recent GCC (4.6.3).
 */
static inline void arch_timer_reg_write(const int access, const int reg, u32 val)
{
	if (access == ARCH_TIMER_PHYS_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mcr p15, 0, %0, c14, c2, 1" : : "r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mcr p15, 0, %0, c14, c2, 0" : : "r" (val));
			break;
		}
	}

	if (access == ARCH_TIMER_VIRT_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mcr p15, 0, %0, c14, c3, 1" : : "r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mcr p15, 0, %0, c14, c3, 0" : : "r" (val));
			break;
		}
	}

	isb();
}

static inline u32 arch_timer_reg_read(const int access, const int reg)
{
	u32 val = 0;

	if (access == ARCH_TIMER_PHYS_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mrc p15, 0, %0, c14, c2, 0" : "=r" (val));
			break;
		}
	}

	if (access == ARCH_TIMER_VIRT_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mrc p15, 0, %0, c14, c3, 1" : "=r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mrc p15, 0, %0, c14, c3, 0" : "=r" (val));
			break;
		}
	}

	return val;
}

static inline u32 arch_timer_get_cntfrq(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (val));
	return val;
}

static inline u64 arch_counter_get_cntpct(void)
{
	u64 cval;
	asm volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r" (cval));
	return cval;
}

static inline u64 arch_counter_get_cntvct(void)
{
	u64 cval;
	asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r" (cval));
	return cval;
}

static irqreturn_t inline timer_handler(const int access,
					struct clock_event_device *evt)
{
	unsigned long ctrl;
	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL);
	if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {
		ctrl |= ARCH_TIMER_CTRL_IT_MASK;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl);
		evt->event_handler(evt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static irqreturn_t arch_timer_handler_virt(int irq, void *dev_id)
{
	struct clock_event_device *evt = *(struct clock_event_device **)dev_id;

	return timer_handler(ARCH_TIMER_VIRT_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_phys(int irq, void *dev_id)
{
	struct clock_event_device *evt = *(struct clock_event_device **)dev_id;

	return timer_handler(ARCH_TIMER_PHYS_ACCESS, evt);
}

static inline void timer_set_mode(const int access, int mode)
{
	unsigned long ctrl;
	switch (mode) {
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
		ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL);
		ctrl &= ~ARCH_TIMER_CTRL_ENABLE;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl);
		break;
	default:
		break;
	}
}

static void arch_timer_set_mode_virt(enum clock_event_mode mode,
				     struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_VIRT_ACCESS, mode);
}

static void arch_timer_set_mode_phys(enum clock_event_mode mode,
				     struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_PHYS_ACCESS, mode);
}

static inline void set_next_event(const int access, unsigned long evt)
{
	unsigned long ctrl;
	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL);
	ctrl |= ARCH_TIMER_CTRL_ENABLE;
	ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
	arch_timer_reg_write(access, ARCH_TIMER_REG_TVAL, evt);
	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl);
}

static int arch_timer_set_next_event_virt(unsigned long evt,
					  struct clock_event_device *unused)
{
	set_next_event(ARCH_TIMER_VIRT_ACCESS, evt);
	return 0;
}

static int arch_timer_set_next_event_phys(unsigned long evt,
					  struct clock_event_device *unused)
{
	set_next_event(ARCH_TIMER_PHYS_ACCESS, evt);
	return 0;
}

static int __cpuinit arch_timer_setup(struct clock_event_device *clk)
{
	clk->features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_C3STOP;
	clk->name = "arch_sys_timer";
	clk->rating = 450;
	if (arch_timer_use_virtual) {
		clk->irq = arch_timer_ppi[VIRT_PPI];
		clk->set_mode = arch_timer_set_mode_virt;
		clk->set_next_event = arch_timer_set_next_event_virt;
	} else {
		clk->irq = arch_timer_ppi[PHYS_SECURE_PPI];
		clk->set_mode = arch_timer_set_mode_phys;
		clk->set_next_event = arch_timer_set_next_event_phys;
	}

	clk->set_mode(CLOCK_EVT_MODE_SHUTDOWN, NULL);

	clockevents_config_and_register(clk, arch_timer_rate,
					0xf, 0x7fffffff);

	*__this_cpu_ptr(arch_timer_evt) = clk;

	if (arch_timer_use_virtual)
		enable_percpu_irq(arch_timer_ppi[VIRT_PPI], 0);
	else {
		enable_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI], 0);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			enable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI], 0);
	}

	return 0;
}

static int arch_timer_available(void)
{
	u32 freq;

	if (arch_timer_rate == 0) {
		freq = arch_timer_get_cntfrq();

		/* Check the timer frequency. */
		if (freq == 0) {
			pr_warn("Architected timer frequency not available\n");
			return -EINVAL;
		}

		arch_timer_rate = freq;
	}

	pr_info_once("Architected local timer running at %lu.%02luMHz (%s).\n",
		     (unsigned long)arch_timer_rate / 1000000,
		     (unsigned long)(arch_timer_rate / 10000) % 100,
		     arch_timer_use_virtual ? "virt" : "phys");
	return 0;
}

/*
 * Some external users of arch_timer_read_counter (e.g. sched_clock) may try to
 * call it before it has been initialised. Rather than incur a performance
 * penalty checking for initialisation, provide a default implementation that
 * won't lead to time appearing to jump backwards.
 */
static u64 arch_timer_read_zero(void)
{
	return 0;
}

u64 (*arch_timer_read_counter)(void) = arch_timer_read_zero;

static u32 arch_timer_read_counter32(void)
{
	return arch_timer_read_counter();
}

static cycle_t arch_counter_read(struct clocksource *cs)
{
	return arch_timer_read_counter();
}

static unsigned long arch_timer_read_current_timer(void)
{
	return arch_timer_read_counter();
}

static cycle_t arch_counter_read_cc(const struct cyclecounter *cc)
{
	return arch_timer_read_counter();
}

static struct clocksource clocksource_counter = {
	.name	= "arch_sys_counter",
	.rating	= 400,
	.read	= arch_counter_read,
	.mask	= CLOCKSOURCE_MASK(56),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static struct cyclecounter cyclecounter = {
	.read	= arch_counter_read_cc,
	.mask	= CLOCKSOURCE_MASK(56),
};

static struct timecounter timecounter;

struct timecounter *arch_timer_get_timecounter(void)
{
	return &timecounter;
}

static void __cpuinit arch_timer_stop(struct clock_event_device *clk)
{
	pr_debug("arch_timer_teardown disable IRQ%d cpu #%d\n",
		 clk->irq, smp_processor_id());

	if (arch_timer_use_virtual)
		disable_percpu_irq(arch_timer_ppi[VIRT_PPI]);
	else {
		disable_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI]);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			disable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI]);
	}

	clk->set_mode(CLOCK_EVT_MODE_UNUSED, clk);
}

static struct local_timer_ops arch_timer_ops __cpuinitdata = {
	.setup	= arch_timer_setup,
	.stop	= arch_timer_stop,
};

static struct clock_event_device arch_timer_global_evt;

static int __init arch_timer_register(void)
{
	int err;
	int ppi;

	err = arch_timer_available();
	if (err)
		goto out;

	arch_timer_evt = alloc_percpu(struct clock_event_device *);
	if (!arch_timer_evt) {
		err = -ENOMEM;
		goto out;
	}

	clocksource_register_hz(&clocksource_counter, arch_timer_rate);
	cyclecounter.mult = clocksource_counter.mult;
	cyclecounter.shift = clocksource_counter.shift;
	timecounter_init(&timecounter, &cyclecounter,
			 arch_counter_get_cntpct());

	if (arch_timer_use_virtual) {
		ppi = arch_timer_ppi[VIRT_PPI];
		err = request_percpu_irq(ppi, arch_timer_handler_virt,
					 "arch_timer", arch_timer_evt);
	} else {
		ppi = arch_timer_ppi[PHYS_SECURE_PPI];
		err = request_percpu_irq(ppi, arch_timer_handler_phys,
					 "arch_timer", arch_timer_evt);
		if (!err && arch_timer_ppi[PHYS_NONSECURE_PPI]) {
			ppi = arch_timer_ppi[PHYS_NONSECURE_PPI];
			err = request_percpu_irq(ppi, arch_timer_handler_phys,
						 "arch_timer", arch_timer_evt);
			if (err)
				free_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI],
						arch_timer_evt);
		}
	}

	if (err) {
		pr_err("arch_timer: can't register interrupt %d (%d)\n",
		       ppi, err);
		goto out_free;
	}

	err = local_timer_register(&arch_timer_ops);
	if (err) {
		/*
		 * We couldn't register as a local timer (could be
		 * because we're on a UP platform, or because some
		 * other local timer is already present...). Try as a
		 * global timer instead.
		 */
		arch_timer_global_evt.cpumask = cpumask_of(0);
		err = arch_timer_setup(&arch_timer_global_evt);
	}
	if (err)
		goto out_free_irq;

	/* Use the architected timer for the delay loop. */
	arch_delay_timer.read_current_timer = &arch_timer_read_current_timer;
	arch_delay_timer.freq = arch_timer_rate;
	register_current_timer_delay(&arch_delay_timer);
	return 0;

out_free_irq:
	if (arch_timer_use_virtual)
		free_percpu_irq(arch_timer_ppi[VIRT_PPI], arch_timer_evt);
	else {
		free_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI],
				arch_timer_evt);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			free_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI],
					arch_timer_evt);
	}

out_free:
	free_percpu(arch_timer_evt);
out:
	return err;
}

static const struct of_device_id arch_timer_of_match[] __initconst = {
	{ .compatible	= "arm,armv7-timer",	},
	{},
};

int __init arch_timer_of_register(void)
{
	struct device_node *np;
	u32 freq;
	int i;

	np = of_find_matching_node(NULL, arch_timer_of_match);
	if (!np) {
		pr_err("arch_timer: can't find DT node\n");
		return -ENODEV;
	}

	/* Try to determine the frequency from the device tree or CNTFRQ */
	if (!of_property_read_u32(np, "clock-frequency", &freq))
		arch_timer_rate = freq;

	for (i = PHYS_SECURE_PPI; i < MAX_TIMER_PPI; i++)
		arch_timer_ppi[i] = irq_of_parse_and_map(np, i);

	of_node_put(np);

	/*
	 * If no interrupt provided for virtual timer, we'll have to
	 * stick to the physical timer. It'd better be accessible...
	 */
	if (!arch_timer_ppi[VIRT_PPI]) {
		arch_timer_use_virtual = false;

		if (!arch_timer_ppi[PHYS_SECURE_PPI] ||
		    !arch_timer_ppi[PHYS_NONSECURE_PPI]) {
			pr_warn("arch_timer: No interrupt available, giving up\n");
			return -EINVAL;
		}
	}

	if (arch_timer_use_virtual)
		arch_timer_read_counter = arch_counter_get_cntvct;
	else
		arch_timer_read_counter = arch_counter_get_cntpct;

	return arch_timer_register();
}

int __init arch_timer_sched_clock_init(void)
{
	int err;

	err = arch_timer_available();
	if (err)
		return err;

	setup_sched_clock(arch_timer_read_counter32,
			  32, arch_timer_rate);
	return 0;
}
