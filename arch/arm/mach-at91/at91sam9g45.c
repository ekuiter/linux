/*
 *  Chip-specific setup code for the AT91SAM9G45 family
 *
 *  Copyright (C) 2009 Atmel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <asm/system_misc.h>
#include <mach/hardware.h>

#include "soc.h"
#include "generic.h"

/* --------------------------------------------------------------------
 *  AT91SAM9G45 processor initialization
 * -------------------------------------------------------------------- */
static void __init at91sam9g45_initialize(void)
{
	arm_pm_idle = at91sam9_idle;
}

AT91_SOC_START(at91sam9g45)
	.init = at91sam9g45_initialize,
AT91_SOC_END
