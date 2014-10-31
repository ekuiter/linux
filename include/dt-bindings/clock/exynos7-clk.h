/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Naveen Krishna Ch <naveenkrishna.ch@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#ifndef _DT_BINDINGS_CLOCK_EXYNOS7_H
#define _DT_BINDINGS_CLOCK_EXYNOS7_H

/* TOPC */
#define DOUT_ACLK_PERIS			1
#define DOUT_SCLK_BUS0_PLL		2
#define DOUT_SCLK_BUS1_PLL		3
#define DOUT_SCLK_CC_PLL		4
#define DOUT_SCLK_MFC_PLL		5
#define TOPC_NR_CLK			6

/* TOP0 */
#define DOUT_ACLK_PERIC1		1
#define DOUT_ACLK_PERIC0		2
#define CLK_SCLK_UART0			3
#define CLK_SCLK_UART1			4
#define CLK_SCLK_UART2			5
#define CLK_SCLK_UART3			6
#define TOP0_NR_CLK			7

/* PERIC0 */
#define PCLK_UART0			1
#define SCLK_UART0			2
#define PERIC0_NR_CLK			3

/* PERIC1 */
#define PCLK_UART1			1
#define PCLK_UART2			2
#define PCLK_UART3			3
#define SCLK_UART1			4
#define SCLK_UART2			5
#define SCLK_UART3			6
#define PERIC1_NR_CLK			7

/* PERIS */
#define PCLK_CHIPID			1
#define SCLK_CHIPID			2
#define PERIS_NR_CLK			3

#endif /* _DT_BINDINGS_CLOCK_EXYNOS7_H */
