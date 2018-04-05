/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_wrapper.h - x86 specific wrappers to syscall definitions
 */

#ifndef _ASM_X86_SYSCALL_WRAPPER_H
#define _ASM_X86_SYSCALL_WRAPPER_H

/* Mapping of registers to parameters for syscalls on x86-64 and x32 */
#define SC_X86_64_REGS_TO_ARGS(x, ...)					\
	__MAP(x,__SC_ARGS						\
		,,regs->di,,regs->si,,regs->dx				\
		,,regs->r10,,regs->r8,,regs->r9)			\

/* Mapping of registers to parameters for syscalls on i386 */
#define SC_IA32_REGS_TO_ARGS(x, ...)					\
	__MAP(x,__SC_ARGS						\
	      ,,(unsigned int)regs->bx,,(unsigned int)regs->cx		\
	      ,,(unsigned int)regs->dx,,(unsigned int)regs->si		\
	      ,,(unsigned int)regs->di,,(unsigned int)regs->bp)

#ifdef CONFIG_IA32_EMULATION
/*
 * For IA32 emulation, we need to handle "compat" syscalls *and* create
 * additional wrappers (aptly named __sys_ia32_sys_xyzzy) which decode the
 * ia32 regs in the proper order for shared or "common" syscalls. As some
 * syscalls may not be implemented, we need to expand COND_SYSCALL in
 * kernel/sys_ni.c and SYS_NI in kernel/time/posix-stubs.c to cover this
 * case as well.
 */
#define COMPAT_SC_IA32_STUBx(x, name, ...)				\
	asmlinkage long __compat_sys_ia32##name(const struct pt_regs *regs);\
	ALLOW_ERROR_INJECTION(__compat_sys_ia32##name, ERRNO);		\
	asmlinkage long __compat_sys_ia32##name(const struct pt_regs *regs)\
	{								\
		return c_SyS##name(SC_IA32_REGS_TO_ARGS(x,__VA_ARGS__));\
	}								\

#define SC_IA32_WRAPPERx(x, name, ...)					\
	asmlinkage long __sys_ia32##name(const struct pt_regs *regs);	\
	ALLOW_ERROR_INJECTION(__sys_ia32##name, ERRNO);			\
	asmlinkage long __sys_ia32##name(const struct pt_regs *regs)	\
	{								\
		return SyS##name(SC_IA32_REGS_TO_ARGS(x,__VA_ARGS__));	\
	}

#define COND_SYSCALL(name)						\
	cond_syscall(sys_##name);					\
	cond_syscall(__sys_ia32_##name)

#define SYS_NI(name)							\
	SYSCALL_ALIAS(sys_##name, sys_ni_posix_timers);			\
	SYSCALL_ALIAS(__sys_ia32_##name, sys_ni_posix_timers)

#else /* CONFIG_IA32_EMULATION */
#define COMPAT_SC_IA32_STUBx(x, name, ...)
#define SC_IA32_WRAPPERx(x, fullname, name, ...)
#endif /* CONFIG_IA32_EMULATION */


#ifdef CONFIG_X86_X32
/*
 * For the x32 ABI, we need to create a stub for compat_sys_*() which is aware
 * of the x86-64-style parameter ordering of x32 syscalls. The syscalls common
 * with x86_64 obviously do not need such care.
 */
#define COMPAT_SC_X32_STUBx(x, name, ...)				\
	asmlinkage long __compat_sys_x32##name(const struct pt_regs *regs);\
	ALLOW_ERROR_INJECTION(__compat_sys_x32##name, ERRNO);		\
	asmlinkage long __compat_sys_x32##name(const struct pt_regs *regs)\
	{								\
		return c_SyS##name(SC_X86_64_REGS_TO_ARGS(x,__VA_ARGS__));\
	}								\

#else /* CONFIG_X86_X32 */
#define COMPAT_SC_X32_STUBx(x, name, ...)
#endif /* CONFIG_X86_X32 */


#ifdef CONFIG_COMPAT
/*
 * Compat means IA32_EMULATION and/or X86_X32. As they use a different
 * mapping of registers to parameters, we need to generate stubs for each
 * of them. There is no need to implement COMPAT_SYSCALL_DEFINE0, as it is
 * unused on x86.
 */
#define COMPAT_SYSCALL_DEFINEx(x, name, ...)				\
	static long c_SyS##name(__MAP(x,__SC_LONG,__VA_ARGS__));	\
	static inline long C_SYSC##name(__MAP(x,__SC_DECL,__VA_ARGS__));\
	COMPAT_SC_IA32_STUBx(x, name, __VA_ARGS__)			\
	COMPAT_SC_X32_STUBx(x, name, __VA_ARGS__)			\
	static long c_SyS##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{								\
		return C_SYSC##name(__MAP(x,__SC_DELOUSE,__VA_ARGS__));	\
	}								\
	static inline long C_SYSC##name(__MAP(x,__SC_DECL,__VA_ARGS__))

/*
 * As some compat syscalls may not be implemented, we need to expand
 * COND_SYSCALL_COMPAT in kernel/sys_ni.c and COMPAT_SYS_NI in
 * kernel/time/posix-stubs.c to cover this case as well.
 */
#define COND_SYSCALL_COMPAT(name) 					\
	cond_syscall(__compat_sys_ia32_##name);				\
	cond_syscall(__compat_sys_x32_##name)

#define COMPAT_SYS_NI(name)						\
	SYSCALL_ALIAS(__compat_sys_ia32_##name, sys_ni_posix_timers);	\
	SYSCALL_ALIAS(__compat_sys_x32_##name, sys_ni_posix_timers)

#endif /* CONFIG_COMPAT */


/*
 * Instead of the generic __SYSCALL_DEFINEx() definition, this macro takes
 * struct pt_regs *regs as the only argument of the syscall stub named
 * sys_*(). It decodes just the registers it needs and passes them on to
 * the SyS_*() wrapper and then to the SYSC_*() function doing the actual job.
 * These wrappers and functions are inlined, meaning that the assembly looks
 * as follows (slightly re-ordered):
 *
 * <sys_recv>:			<-- syscall with 4 parameters
 *	callq	<__fentry__>
 *
 *	mov	0x70(%rdi),%rdi	<-- decode regs->di
 *	mov	0x68(%rdi),%rsi	<-- decode regs->si
 *	mov	0x60(%rdi),%rdx	<-- decode regs->dx
 *	mov	0x38(%rdi),%rcx	<-- decode regs->r10
 *
 *	xor	%r9d,%r9d	<-- clear %r9
 *	xor	%r8d,%r8d	<-- clear %r8
 *
 *	callq	__sys_recvfrom	<-- do the actual work in __sys_recvfrom()
 *				    which takes 6 arguments
 *
 *	cltq			<-- extend return value to 64-bit
 *	retq			<-- return
 *
 * This approach avoids leaking random user-provided register content down
 * the call chain.
 *
 * If IA32_EMULATION is enabled, this macro generates an additional wrapper
 * named __sys_ia32_*() which decodes the struct pt_regs *regs according
 * to the i386 calling convention (bx, cx, dx, si, di, bp).
 *
 * As the generic SYSCALL_DEFINE0() macro does not decode any parameters for
 * obvious reasons, and passing struct pt_regs *regs to it in %rdi does not
 * hurt, there is no need to override it, or to define it differently for
 * IA32_EMULATION.
 */
#define __SYSCALL_DEFINEx(x, name, ...)					\
	asmlinkage long sys##name(const struct pt_regs *regs);		\
	ALLOW_ERROR_INJECTION(sys##name, ERRNO);			\
	static long SyS##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
	static inline long SYSC##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
	asmlinkage long sys##name(const struct pt_regs *regs)		\
	{								\
		return SyS##name(SC_X86_64_REGS_TO_ARGS(x,__VA_ARGS__));\
	}								\
	SC_IA32_WRAPPERx(x, name, __VA_ARGS__)				\
	static long SyS##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{								\
		long ret = SYSC##name(__MAP(x,__SC_CAST,__VA_ARGS__));	\
		__MAP(x,__SC_TEST,__VA_ARGS__);				\
		__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));	\
		return ret;						\
	}								\
	static inline long SYSC##name(__MAP(x,__SC_DECL,__VA_ARGS__))

/*
 * For VSYSCALLS, we need to declare these three syscalls with the new
 * pt_regs-based calling convention for in-kernel use.
 */
struct pt_regs;
asmlinkage long sys_getcpu(const struct pt_regs *regs);		/* di,si,dx */
asmlinkage long sys_gettimeofday(const struct pt_regs *regs);	/* di,si */
asmlinkage long sys_time(const struct pt_regs *regs);		/* di */

#endif /* _ASM_X86_SYSCALL_WRAPPER_H */
