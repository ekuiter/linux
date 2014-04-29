/*
 *	Just a place holder. 
 */
#ifndef _SPARC_SETUP_H
#define _SPARC_SETUP_H

#include <linux/interrupt.h>

#include <uapi/asm/setup.h>

extern char reboot_command[];

#ifdef CONFIG_SPARC32
/* The CPU that was used for booting
 * Only sun4d + leon may have boot_cpu_id != 0
 */
extern unsigned char boot_cpu_id;

extern unsigned long empty_zero_page;

extern int serial_console;
static inline int con_is_present(void)
{
	return serial_console ? 0 : 1;
}

/* from irq_32.c */
extern volatile unsigned char *fdc_status;
extern char *pdma_vaddr;
extern unsigned long pdma_size;
extern volatile int doing_pdma;

/* This is software state */
extern char *pdma_base;
extern unsigned long pdma_areasize;

int sparc_floppy_request_irq(unsigned int irq, irq_handler_t irq_handler);

#endif

extern void sun_do_break(void);
extern int stop_a_enabled;
extern int scons_pwroff;

#endif /* _SPARC_SETUP_H */
