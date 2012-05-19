#ifndef __ASM_SH_CPU_SH4_DMA_SH7780_H
#define __ASM_SH_CPU_SH4_DMA_SH7780_H

#include <linux/sh_intc.h>

#if defined(CONFIG_CPU_SUBTYPE_SH7343) || \
	defined(CONFIG_CPU_SUBTYPE_SH7730)
#define DMTE0_IRQ	evt2irq(0x800)
#define DMTE4_IRQ	evt2irq(0xb80)
#define DMAE0_IRQ	evt2irq(0xbc0)	/* DMA Error IRQ*/
#define SH_DMAC_BASE0	0xFE008020
#define SH_DMARS_BASE0	0xFE009000
#elif defined(CONFIG_CPU_SUBTYPE_SH7722)
#define DMTE0_IRQ	evt2irq(0x800)
#define DMTE4_IRQ	evt2irq(0xb80)
#define DMAE0_IRQ	evt2irq(0xbc0)	/* DMA Error IRQ*/
#define SH_DMAC_BASE0	0xFE008020
#define SH_DMARS_BASE0	0xFE009000
#elif defined(CONFIG_CPU_SUBTYPE_SH7763) || \
	defined(CONFIG_CPU_SUBTYPE_SH7764)
#define DMTE0_IRQ	evt2irq(0x640)
#define DMTE4_IRQ	evt2irq(0x780)
#define DMAE0_IRQ	evt2irq(0x6c0)
#define SH_DMAC_BASE0	0xFF608020
#define SH_DMARS_BASE0	0xFF609000
#elif defined(CONFIG_CPU_SUBTYPE_SH7723)
#define DMTE0_IRQ	evt2irq(0x800)	/* DMAC0A*/
#define DMTE4_IRQ	evt2irq(0xb80)	/* DMAC0B */
#define DMTE6_IRQ	evt2irq(0x700)
#define DMTE8_IRQ	evt2irq(0x740)	/* DMAC1A */
#define DMTE9_IRQ	evt2irq(0x760)
#define DMTE10_IRQ	evt2irq(0xb00)	/* DMAC1B */
#define DMTE11_IRQ	evt2irq(0xb20)
#define DMAE0_IRQ	evt2irq(0xbc0)	/* DMA Error IRQ*/
#define DMAE1_IRQ	evt2irq(0xb40)	/* DMA Error IRQ*/
#define SH_DMAC_BASE0	0xFE008020
#define SH_DMAC_BASE1	0xFDC08020
#define SH_DMARS_BASE0	0xFDC09000
#elif defined(CONFIG_CPU_SUBTYPE_SH7724)
#define DMTE0_IRQ	evt2irq(0x800)	/* DMAC0A*/
#define DMTE4_IRQ	evt2irq(0xb80)	/* DMAC0B */
#define DMTE6_IRQ	evt2irq(0x700)
#define DMTE8_IRQ	evt2irq(0x740)	/* DMAC1A */
#define DMTE9_IRQ	evt2irq(0x760)
#define DMTE10_IRQ	evt2irq(0xb00)	/* DMAC1B */
#define DMTE11_IRQ	evt2irq(0xb20)
#define DMAE0_IRQ	evt2irq(0xbc0)	/* DMA Error IRQ*/
#define DMAE1_IRQ	evt2irq(0xb40)	/* DMA Error IRQ*/
#define SH_DMAC_BASE0	0xFE008020
#define SH_DMAC_BASE1	0xFDC08020
#define SH_DMARS_BASE0	0xFE009000
#define SH_DMARS_BASE1	0xFDC09000
#elif defined(CONFIG_CPU_SUBTYPE_SH7780)
#define DMTE0_IRQ	evt2irq(0x640)
#define DMTE4_IRQ	evt2irq(0x780)
#define DMTE6_IRQ	evt2irq(0x7c0)
#define DMTE8_IRQ	evt2irq(0xd80)
#define DMTE9_IRQ	evt2irq(0xda0)
#define DMTE10_IRQ	evt2irq(0xdc0)
#define DMTE11_IRQ	evt2irq(0xde0)
#define DMAE0_IRQ	evt2irq(0x6c0)	/* DMA Error IRQ */
#define SH_DMAC_BASE0	0xFC808020
#define SH_DMAC_BASE1	0xFC818020
#define SH_DMARS_BASE0	0xFC809000
#else /* SH7785 */
#define DMTE0_IRQ	evt2irq(0x620)
#define DMTE4_IRQ	evt2irq(0x6a0)
#define DMTE6_IRQ	evt2irq(0x880)
#define DMTE8_IRQ	evt2irq(0x8c0)
#define DMTE9_IRQ	evt2irq(0x8e0)
#define DMTE10_IRQ	evt2irq(0x900)
#define DMTE11_IRQ	evt2irq(0x920)
#define DMAE0_IRQ	evt2irq(0x6e0)	/* DMA Error IRQ0 */
#define DMAE1_IRQ	evt2irq(0x940)	/* DMA Error IRQ1 */
#define SH_DMAC_BASE0	0xFC808020
#define SH_DMAC_BASE1	0xFCC08020
#define SH_DMARS_BASE0	0xFC809000
#endif

#define REQ_HE		0x000000C0
#define REQ_H		0x00000080
#define REQ_LE		0x00000040
#define TM_BURST	0x00000020

#endif /* __ASM_SH_CPU_SH4_DMA_SH7780_H */
