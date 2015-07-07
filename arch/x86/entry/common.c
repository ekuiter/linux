/*
 * common.c - C code for kernel entry and exit
 * Copyright (c) 2015 Andrew Lutomirski
 * GPL v2
 *
 * Based on asm and ptrace code by many authors.  The code here originated
 * in ptrace.c and signal.c.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/tracehook.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/signal.h>
#include <linux/export.h>
#include <linux/context_tracking.h>
#include <linux/user-return-notifier.h>
#include <linux/uprobes.h>

#include <asm/desc.h>
#include <asm/traps.h>

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

#ifdef CONFIG_CONTEXT_TRACKING
/* Called on entry from user mode with IRQs off. */
__visible void enter_from_user_mode(void)
{
	CT_WARN_ON(ct_state() != CONTEXT_USER);
	user_exit();
}
#endif

static void do_audit_syscall_entry(struct pt_regs *regs, u32 arch)
{
#ifdef CONFIG_X86_64
	if (arch == AUDIT_ARCH_X86_64) {
		audit_syscall_entry(regs->orig_ax, regs->di,
				    regs->si, regs->dx, regs->r10);
	} else
#endif
	{
		audit_syscall_entry(regs->orig_ax, regs->bx,
				    regs->cx, regs->dx, regs->si);
	}
}

/*
 * We can return 0 to resume the syscall or anything else to go to phase
 * 2.  If we resume the syscall, we need to put something appropriate in
 * regs->orig_ax.
 *
 * NB: We don't have full pt_regs here, but regs->orig_ax and regs->ax
 * are fully functional.
 *
 * For phase 2's benefit, our return value is:
 * 0:			resume the syscall
 * 1:			go to phase 2; no seccomp phase 2 needed
 * anything else:	go to phase 2; pass return value to seccomp
 */
unsigned long syscall_trace_enter_phase1(struct pt_regs *regs, u32 arch)
{
	unsigned long ret = 0;
	u32 work;

	BUG_ON(regs != task_pt_regs(current));

	work = ACCESS_ONCE(current_thread_info()->flags) &
		_TIF_WORK_SYSCALL_ENTRY;

#ifdef CONFIG_CONTEXT_TRACKING
	/*
	 * If TIF_NOHZ is set, we are required to call user_exit() before
	 * doing anything that could touch RCU.
	 */
	if (work & _TIF_NOHZ) {
		enter_from_user_mode();
		work &= ~_TIF_NOHZ;
	}
#endif

#ifdef CONFIG_SECCOMP
	/*
	 * Do seccomp first -- it should minimize exposure of other
	 * code, and keeping seccomp fast is probably more valuable
	 * than the rest of this.
	 */
	if (work & _TIF_SECCOMP) {
		struct seccomp_data sd;

		sd.arch = arch;
		sd.nr = regs->orig_ax;
		sd.instruction_pointer = regs->ip;
#ifdef CONFIG_X86_64
		if (arch == AUDIT_ARCH_X86_64) {
			sd.args[0] = regs->di;
			sd.args[1] = regs->si;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->r10;
			sd.args[4] = regs->r8;
			sd.args[5] = regs->r9;
		} else
#endif
		{
			sd.args[0] = regs->bx;
			sd.args[1] = regs->cx;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->si;
			sd.args[4] = regs->di;
			sd.args[5] = regs->bp;
		}

		BUILD_BUG_ON(SECCOMP_PHASE1_OK != 0);
		BUILD_BUG_ON(SECCOMP_PHASE1_SKIP != 1);

		ret = seccomp_phase1(&sd);
		if (ret == SECCOMP_PHASE1_SKIP) {
			regs->orig_ax = -1;
			ret = 0;
		} else if (ret != SECCOMP_PHASE1_OK) {
			return ret;  /* Go directly to phase 2 */
		}

		work &= ~_TIF_SECCOMP;
	}
#endif

	/* Do our best to finish without phase 2. */
	if (work == 0)
		return ret;  /* seccomp and/or nohz only (ret == 0 here) */

#ifdef CONFIG_AUDITSYSCALL
	if (work == _TIF_SYSCALL_AUDIT) {
		/*
		 * If there is no more work to be done except auditing,
		 * then audit in phase 1.  Phase 2 always audits, so, if
		 * we audit here, then we can't go on to phase 2.
		 */
		do_audit_syscall_entry(regs, arch);
		return 0;
	}
#endif

	return 1;  /* Something is enabled that we can't handle in phase 1 */
}

/* Returns the syscall nr to run (which should match regs->orig_ax). */
long syscall_trace_enter_phase2(struct pt_regs *regs, u32 arch,
				unsigned long phase1_result)
{
	long ret = 0;
	u32 work = ACCESS_ONCE(current_thread_info()->flags) &
		_TIF_WORK_SYSCALL_ENTRY;

	BUG_ON(regs != task_pt_regs(current));

	/*
	 * If we stepped into a sysenter/syscall insn, it trapped in
	 * kernel mode; do_debug() cleared TF and set TIF_SINGLESTEP.
	 * If user-mode had set TF itself, then it's still clear from
	 * do_debug() and we need to set it again to restore the user
	 * state.  If we entered on the slow path, TF was already set.
	 */
	if (work & _TIF_SINGLESTEP)
		regs->flags |= X86_EFLAGS_TF;

#ifdef CONFIG_SECCOMP
	/*
	 * Call seccomp_phase2 before running the other hooks so that
	 * they can see any changes made by a seccomp tracer.
	 */
	if (phase1_result > 1 && seccomp_phase2(phase1_result)) {
		/* seccomp failures shouldn't expose any additional code. */
		return -1;
	}
#endif

	if (unlikely(work & _TIF_SYSCALL_EMU))
		ret = -1L;

	if ((ret || test_thread_flag(TIF_SYSCALL_TRACE)) &&
	    tracehook_report_syscall_entry(regs))
		ret = -1L;

	if (unlikely(test_thread_flag(TIF_SYSCALL_TRACEPOINT)))
		trace_sys_enter(regs, regs->orig_ax);

	do_audit_syscall_entry(regs, arch);

	return ret ?: regs->orig_ax;
}

long syscall_trace_enter(struct pt_regs *regs)
{
	u32 arch = is_ia32_task() ? AUDIT_ARCH_I386 : AUDIT_ARCH_X86_64;
	unsigned long phase1_result = syscall_trace_enter_phase1(regs, arch);

	if (phase1_result == 0)
		return regs->orig_ax;
	else
		return syscall_trace_enter_phase2(regs, arch, phase1_result);
}

void syscall_trace_leave(struct pt_regs *regs)
{
	bool step;

	/*
	 * We may come here right after calling schedule_user()
	 * or do_notify_resume(), in which case we can be in RCU
	 * user mode.
	 */
	user_exit();

	audit_syscall_exit(regs);

	if (unlikely(test_thread_flag(TIF_SYSCALL_TRACEPOINT)))
		trace_sys_exit(regs, regs->ax);

	/*
	 * If TIF_SYSCALL_EMU is set, we only get here because of
	 * TIF_SINGLESTEP (i.e. this is PTRACE_SYSEMU_SINGLESTEP).
	 * We already reported this syscall instruction in
	 * syscall_trace_enter().
	 */
	step = unlikely(test_thread_flag(TIF_SINGLESTEP)) &&
			!test_thread_flag(TIF_SYSCALL_EMU);
	if (step || test_thread_flag(TIF_SYSCALL_TRACE))
		tracehook_report_syscall_exit(regs, step);

	user_enter();
}

/*
 * notification of userspace execution resumption
 * - triggered by the TIF_WORK_MASK flags
 */
__visible void
do_notify_resume(struct pt_regs *regs, void *unused, __u32 thread_info_flags)
{
	user_exit();

	if (thread_info_flags & _TIF_UPROBE)
		uprobe_notify_resume(regs);

	/* deal with pending signal delivery */
	if (thread_info_flags & _TIF_SIGPENDING)
		do_signal(regs);

	if (thread_info_flags & _TIF_NOTIFY_RESUME) {
		clear_thread_flag(TIF_NOTIFY_RESUME);
		tracehook_notify_resume(regs);
	}
	if (thread_info_flags & _TIF_USER_RETURN_NOTIFY)
		fire_user_return_notifiers();

	user_enter();
}
