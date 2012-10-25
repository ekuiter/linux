/*
 *  Copyright (C) 2000, 2001 Blue Mug, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/types.h>

#include <asm/setup.h>
#include <asm/mach/map.h>
#include <asm/mach/arch.h>
#include <asm/mach-types.h>

#include <mach/hardware.h>

#include "common.h"

#define VIDEORAM_SIZE	SZ_128K

static struct map_desc edb7211_io_desc[] __initdata = {
	{	/* Memory-mapped extra keyboard row */
		.virtual	= IO_ADDRESS(EP7211_PHYS_EXTKBD),
		.pfn		= __phys_to_pfn(EP7211_PHYS_EXTKBD),
		.length		= SZ_1M,
		.type		= MT_DEVICE,
	}, {	/* CS8900A Ethernet chip */
		.virtual	= IO_ADDRESS(EP7211_PHYS_CS8900A),
		.pfn		= __phys_to_pfn(EP7211_PHYS_CS8900A),
		.length		= SZ_1M,
		.type		= MT_DEVICE,
	}, {	/* Flash bank 0 */
		.virtual	= IO_ADDRESS(EP7211_PHYS_FLASH1),
		.pfn		= __phys_to_pfn(EP7211_PHYS_FLASH1),
		.length		= SZ_8M,
		.type		= MT_DEVICE,
	}, {	/* Flash bank 1 */
		.virtual	= IO_ADDRESS(EP7211_PHYS_FLASH2),
		.pfn		= __phys_to_pfn(EP7211_PHYS_FLASH2),
		.length		= SZ_8M,
		.type		= MT_DEVICE,
	},
};

void __init edb7211_map_io(void)
{
	clps711x_map_io();
	iotable_init(edb7211_io_desc, ARRAY_SIZE(edb7211_io_desc));
}

/* Reserve screen memory region at the start of main system memory. */
static void __init edb7211_reserve(void)
{
	memblock_reserve(PHYS_OFFSET, VIDEORAM_SIZE);
}

static void __init
fixup_edb7211(struct tag *tags, char **cmdline, struct meminfo *mi)
{
	/*
	 * Bank start addresses are not present in the information
	 * passed in from the boot loader.  We could potentially
	 * detect them, but instead we hard-code them.
	 *
	 * Banks sizes _are_ present in the param block, but we're
	 * not using that information yet.
	 */
	mi->bank[0].start = 0xc0000000;
	mi->bank[0].size = SZ_8M;
	mi->bank[1].start = 0xc1000000;
	mi->bank[1].size = SZ_8M;
	mi->nr_banks = 2;
}

MACHINE_START(EDB7211, "CL-EDB7211 (EP7211 eval board)")
	/* Maintainer: Jon McClintock */
	.atag_offset	= VIDEORAM_SIZE + 0x100,
	.fixup		= fixup_edb7211,
	.map_io		= edb7211_map_io,
	.reserve	= edb7211_reserve,
	.init_irq	= clps711x_init_irq,
	.timer		= &clps711x_timer,
	.restart	= clps711x_restart,
MACHINE_END
