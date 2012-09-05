#ifndef _ASM_POWERPC_ABS_ADDR_H
#define _ASM_POWERPC_ABS_ADDR_H
#ifdef __KERNEL__


/*
 * c 2001 PPC 64 Team, IBM Corp
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/memblock.h>

#include <asm/types.h>
#include <asm/page.h>
#include <asm/prom.h>

#define phys_to_abs(pa) (pa)

/* Convenience macros */
#define virt_to_abs(va) __pa(va)
#define abs_to_virt(aa) __va(aa)

#endif /* __KERNEL__ */
#endif /* _ASM_POWERPC_ABS_ADDR_H */
