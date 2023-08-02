/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHSTK_H
#define _ASM_X86_SHSTK_H

#ifndef __ASSEMBLY__
#include <linux/types.h>

struct task_struct;

#ifdef CONFIG_X86_USER_SHADOW_STACK
long shstk_prctl(struct task_struct *task, int option, unsigned long features);
void reset_thread_features(void);
#else
static inline long shstk_prctl(struct task_struct *task, int option,
			       unsigned long arg2) { return -EINVAL; }
static inline void reset_thread_features(void) {}
#endif /* CONFIG_X86_USER_SHADOW_STACK */

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_SHSTK_H */
