/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2000, 2001, 2002 Andi Kleen, SuSE Labs
 */
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/hardirq.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/ftrace.h>
#include <linux/kexec.h>
#include <linux/bug.h>
#include <linux/nmi.h>
#include <linux/sysfs.h>

#include <asm/stacktrace.h>


int panic_on_unrecovered_nmi;
int panic_on_io_nmi;
unsigned int code_bytes = 64;
int kstack_depth_to_print = 3 * STACKSLOTS_PER_LINE;
static int die_counter;

bool in_task_stack(unsigned long *stack, struct task_struct *task,
		   struct stack_info *info)
{
	unsigned long *begin = task_stack_page(task);
	unsigned long *end   = task_stack_page(task) + THREAD_SIZE;

	if (stack < begin || stack >= end)
		return false;

	info->type	= STACK_TYPE_TASK;
	info->begin	= begin;
	info->end	= end;
	info->next_sp	= NULL;

	return true;
}

static void printk_stack_address(unsigned long address, int reliable,
				 char *log_lvl)
{
	touch_nmi_watchdog();
	printk("%s [<%p>] %s%pB\n",
		log_lvl, (void *)address, reliable ? "" : "? ",
		(void *)address);
}

void printk_address(unsigned long address)
{
	pr_cont(" [<%p>] %pS\n", (void *)address, (void *)address);
}

/*
 * x86-64 can have up to three kernel stacks:
 * process stack
 * interrupt stack
 * severe exception (double fault, nmi, stack fault, debug, mce) hardware stack
 */

unsigned long
print_context_stack(struct task_struct *task,
		unsigned long *stack, unsigned long bp,
		const struct stacktrace_ops *ops, void *data,
		struct stack_info *info, int *graph)
{
	struct stack_frame *frame = (struct stack_frame *)bp;

	/*
	 * If we overflowed the stack into a guard page, jump back to the
	 * bottom of the usable stack.
	 */
	if ((unsigned long)task_stack_page(task) - (unsigned long)stack <
	    PAGE_SIZE)
		stack = (unsigned long *)task_stack_page(task);

	while (on_stack(info, stack, sizeof(*stack))) {
		unsigned long addr = *stack;

		if (__kernel_text_address(addr)) {
			unsigned long real_addr;
			int reliable = 0;

			if ((unsigned long) stack == bp + sizeof(long)) {
				reliable = 1;
				frame = frame->next_frame;
				bp = (unsigned long) frame;
			}

			/*
			 * When function graph tracing is enabled for a
			 * function, its return address on the stack is
			 * replaced with the address of an ftrace handler
			 * (return_to_handler).  In that case, before printing
			 * the "real" address, we want to print the handler
			 * address as an "unreliable" hint that function graph
			 * tracing was involved.
			 */
			real_addr = ftrace_graph_ret_addr(task, graph, addr,
							  stack);
			if (real_addr != addr)
				ops->address(data, addr, 0);

			ops->address(data, real_addr, reliable);
		}
		stack++;
	}
	return bp;
}
EXPORT_SYMBOL_GPL(print_context_stack);

unsigned long
print_context_stack_bp(struct task_struct *task,
		       unsigned long *stack, unsigned long bp,
		       const struct stacktrace_ops *ops, void *data,
		       struct stack_info *info, int *graph)
{
	struct stack_frame *frame = (struct stack_frame *)bp;
	unsigned long *retp = &frame->return_address;

	while (on_stack(info, stack, sizeof(*stack) * 2)) {
		unsigned long addr = *retp;
		unsigned long real_addr;

		if (!__kernel_text_address(addr))
			break;

		real_addr = ftrace_graph_ret_addr(task, graph, addr, retp);
		if (ops->address(data, real_addr, 1))
			break;

		frame = frame->next_frame;
		retp = &frame->return_address;
	}

	return (unsigned long)frame;
}
EXPORT_SYMBOL_GPL(print_context_stack_bp);

static int print_trace_stack(void *data, const char *name)
{
	printk("%s <%s> ", (char *)data, name);
	return 0;
}

/*
 * Print one address/symbol entries per line.
 */
static int print_trace_address(void *data, unsigned long addr, int reliable)
{
	printk_stack_address(addr, reliable, data);
	return 0;
}

static const struct stacktrace_ops print_trace_ops = {
	.stack			= print_trace_stack,
	.address		= print_trace_address,
	.walk_stack		= print_context_stack,
};

void
show_trace_log_lvl(struct task_struct *task, struct pt_regs *regs,
		unsigned long *stack, unsigned long bp, char *log_lvl)
{
	printk("%sCall Trace:\n", log_lvl);
	dump_trace(task, regs, stack, bp, &print_trace_ops, log_lvl);
}

void show_stack(struct task_struct *task, unsigned long *sp)
{
	unsigned long bp = 0;

	task = task ? : current;

	/*
	 * Stack frames below this one aren't interesting.  Don't show them
	 * if we're printing for %current.
	 */
	if (!sp && task == current) {
		sp = get_stack_pointer(current, NULL);
		bp = (unsigned long)get_frame_pointer(current, NULL);
	}

	show_stack_log_lvl(task, NULL, sp, bp, "");
}

void show_stack_regs(struct pt_regs *regs)
{
	show_stack_log_lvl(current, regs, NULL, 0, "");
}

static arch_spinlock_t die_lock = __ARCH_SPIN_LOCK_UNLOCKED;
static int die_owner = -1;
static unsigned int die_nest_count;

unsigned long oops_begin(void)
{
	int cpu;
	unsigned long flags;

	oops_enter();

	/* racy, but better than risking deadlock. */
	raw_local_irq_save(flags);
	cpu = smp_processor_id();
	if (!arch_spin_trylock(&die_lock)) {
		if (cpu == die_owner)
			/* nested oops. should stop eventually */;
		else
			arch_spin_lock(&die_lock);
	}
	die_nest_count++;
	die_owner = cpu;
	console_verbose();
	bust_spinlocks(1);
	return flags;
}
EXPORT_SYMBOL_GPL(oops_begin);
NOKPROBE_SYMBOL(oops_begin);

void __noreturn rewind_stack_do_exit(int signr);

void oops_end(unsigned long flags, struct pt_regs *regs, int signr)
{
	if (regs && kexec_should_crash(current))
		crash_kexec(regs);

	bust_spinlocks(0);
	die_owner = -1;
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	die_nest_count--;
	if (!die_nest_count)
		/* Nest count reaches zero, release the lock. */
		arch_spin_unlock(&die_lock);
	raw_local_irq_restore(flags);
	oops_exit();

	if (!signr)
		return;
	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");

	/*
	 * We're not going to return, but we might be on an IST stack or
	 * have very little stack space left.  Rewind the stack and kill
	 * the task.
	 */
	rewind_stack_do_exit(signr);
}
NOKPROBE_SYMBOL(oops_end);

int __die(const char *str, struct pt_regs *regs, long err)
{
#ifdef CONFIG_X86_32
	unsigned short ss;
	unsigned long sp;
#endif
	printk(KERN_DEFAULT
	       "%s: %04lx [#%d]%s%s%s%s\n", str, err & 0xffff, ++die_counter,
	       IS_ENABLED(CONFIG_PREEMPT) ? " PREEMPT"         : "",
	       IS_ENABLED(CONFIG_SMP)     ? " SMP"             : "",
	       debug_pagealloc_enabled()  ? " DEBUG_PAGEALLOC" : "",
	       IS_ENABLED(CONFIG_KASAN)   ? " KASAN"           : "");

	if (notify_die(DIE_OOPS, str, regs, err,
			current->thread.trap_nr, SIGSEGV) == NOTIFY_STOP)
		return 1;

	print_modules();
	show_regs(regs);
#ifdef CONFIG_X86_32
	if (user_mode(regs)) {
		sp = regs->sp;
		ss = regs->ss & 0xffff;
	} else {
		sp = kernel_stack_pointer(regs);
		savesegment(ss, ss);
	}
	printk(KERN_EMERG "EIP: [<%08lx>] ", regs->ip);
	print_symbol("%s", regs->ip);
	printk(" SS:ESP %04x:%08lx\n", ss, sp);
#else
	/* Executive summary in case the oops scrolled away */
	printk(KERN_ALERT "RIP ");
	printk_address(regs->ip);
	printk(" RSP <%016lx>\n", regs->sp);
#endif
	return 0;
}
NOKPROBE_SYMBOL(__die);

/*
 * This is gone through when something in the kernel has done something bad
 * and is about to be terminated:
 */
void die(const char *str, struct pt_regs *regs, long err)
{
	unsigned long flags = oops_begin();
	int sig = SIGSEGV;

	if (!user_mode(regs))
		report_bug(regs->ip, regs);

	if (__die(str, regs, err))
		sig = 0;
	oops_end(flags, regs, sig);
}

static int __init kstack_setup(char *s)
{
	ssize_t ret;
	unsigned long val;

	if (!s)
		return -EINVAL;

	ret = kstrtoul(s, 0, &val);
	if (ret)
		return ret;
	kstack_depth_to_print = val;
	return 0;
}
early_param("kstack", kstack_setup);

static int __init code_bytes_setup(char *s)
{
	ssize_t ret;
	unsigned long val;

	if (!s)
		return -EINVAL;

	ret = kstrtoul(s, 0, &val);
	if (ret)
		return ret;

	code_bytes = val;
	if (code_bytes > 8192)
		code_bytes = 8192;

	return 1;
}
__setup("code_bytes=", code_bytes_setup);
