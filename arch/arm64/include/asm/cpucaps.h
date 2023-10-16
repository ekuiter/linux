/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_CPUCAPS_H
#define __ASM_CPUCAPS_H

#include <asm/cpucap-defs.h>

#ifndef __ASSEMBLY__
#include <linux/types.h>
/*
 * Check whether a cpucap is possible at compiletime.
 */
static __always_inline bool
cpucap_is_possible(const unsigned int cap)
{
	compiletime_assert(__builtin_constant_p(cap),
			   "cap must be a constant");
	compiletime_assert(cap < ARM64_NCAPS,
			   "cap must be < ARM64_NCAPS");

	switch (cap) {
	case ARM64_HAS_PAN:
		return IS_ENABLED(CONFIG_ARM64_PAN);
	case ARM64_SVE:
		return IS_ENABLED(CONFIG_ARM64_SVE);
	case ARM64_SME:
	case ARM64_SME2:
	case ARM64_SME_FA64:
		return IS_ENABLED(CONFIG_ARM64_SME);
	case ARM64_HAS_CNP:
		return IS_ENABLED(CONFIG_ARM64_CNP);
	case ARM64_HAS_ADDRESS_AUTH:
	case ARM64_HAS_GENERIC_AUTH:
		return IS_ENABLED(CONFIG_ARM64_PTR_AUTH);
	case ARM64_HAS_GIC_PRIO_MASKING:
		return IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI);
	case ARM64_MTE:
		return IS_ENABLED(CONFIG_ARM64_MTE);
	case ARM64_BTI:
		return IS_ENABLED(CONFIG_ARM64_BTI);
	case ARM64_HAS_TLB_RANGE:
		return IS_ENABLED(CONFIG_ARM64_TLB_RANGE);
	}

	return true;
}
#endif /* __ASSEMBLY__ */

#endif /* __ASM_CPUCAPS_H */
