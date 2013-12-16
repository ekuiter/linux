/*
 * CPU-measurement facilities
 *
 *  Copyright IBM Corp. 2012
 *  Author(s): Hendrik Brueckner <brueckner@linux.vnet.ibm.com>
 *	       Jan Glauber <jang@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 */
#ifndef _ASM_S390_CPU_MF_H
#define _ASM_S390_CPU_MF_H

#include <linux/errno.h>
#include <asm/facility.h>

#define CPU_MF_INT_SF_IAE	(1 << 31)	/* invalid entry address */
#define CPU_MF_INT_SF_ISE	(1 << 30)	/* incorrect SDBT entry */
#define CPU_MF_INT_SF_PRA	(1 << 29)	/* program request alert */
#define CPU_MF_INT_SF_SACA	(1 << 23)	/* sampler auth. change alert */
#define CPU_MF_INT_SF_LSDA	(1 << 22)	/* loss of sample data alert */
#define CPU_MF_INT_CF_CACA	(1 <<  7)	/* counter auth. change alert */
#define CPU_MF_INT_CF_LCDA	(1 <<  6)	/* loss of counter data alert */
#define CPU_MF_INT_RI_HALTED	(1 <<  5)	/* run-time instr. halted */
#define CPU_MF_INT_RI_BUF_FULL	(1 <<  4)	/* run-time instr. program
						   buffer full */

#define CPU_MF_INT_CF_MASK	(CPU_MF_INT_CF_CACA|CPU_MF_INT_CF_LCDA)
#define CPU_MF_INT_SF_MASK	(CPU_MF_INT_SF_IAE|CPU_MF_INT_SF_ISE|	\
				 CPU_MF_INT_SF_PRA|CPU_MF_INT_SF_SACA|	\
				 CPU_MF_INT_SF_LSDA)
#define CPU_MF_INT_RI_MASK	(CPU_MF_INT_RI_HALTED|CPU_MF_INT_RI_BUF_FULL)

/* CPU measurement facility support */
static inline int cpum_cf_avail(void)
{
	return MACHINE_HAS_LPP && test_facility(67);
}

static inline int cpum_sf_avail(void)
{
	return MACHINE_HAS_LPP && test_facility(68);
}


struct cpumf_ctr_info {
	u16   cfvn;
	u16   auth_ctl;
	u16   enable_ctl;
	u16   act_ctl;
	u16   max_cpu;
	u16   csvn;
	u16   max_cg;
	u16   reserved1;
	u32   reserved2[12];
} __packed;

/* QUERY SAMPLING INFORMATION block */
struct hws_qsi_info_block {	    /* Bit(s) */
	unsigned int b0_13:14;	    /* 0-13: zeros			 */
	unsigned int as:1;	    /* 14: sampling authorisation control*/
	unsigned int b15_21:7;	    /* 15-21: zeros			 */
	unsigned int es:1;	    /* 22: sampling enable control	 */
	unsigned int b23_29:7;	    /* 23-29: zeros			 */
	unsigned int cs:1;	    /* 30: sampling activation control	 */
	unsigned int:1; 	    /* 31: reserved			 */
	unsigned int bsdes:16;	    /* 4-5: size of basic sampling entry      */
	unsigned int dsdes:16;	    /* 6-7: size of diagnostic sampling entry */
	unsigned long min_sampl_rate; /* 8-15: minimum sampling interval */
	unsigned long max_sampl_rate; /* 16-23: maximum sampling interval*/
	unsigned long tear;	    /* 24-31: TEAR contents		 */
	unsigned long dear;	    /* 32-39: DEAR contents		 */
	unsigned int rsvrd0;	    /* 40-43: reserved			 */
	unsigned int cpu_speed;     /* 44-47: CPU speed 		 */
	unsigned long long rsvrd1;  /* 48-55: reserved			 */
	unsigned long long rsvrd2;  /* 56-63: reserved			 */
} __packed;

/* SET SAMPLING CONTROLS request block */
struct hws_lsctl_request_block {
	unsigned int s:1;	    /* 0: maximum buffer indicator	 */
	unsigned int h:1;	    /* 1: part. level reserved for VM use*/
	unsigned long long b2_53:52;/* 2-53: zeros			 */
	unsigned int es:1;	    /* 54: sampling enable control	 */
	unsigned int b55_61:7;	    /* 55-61: - zeros			 */
	unsigned int cs:1;	    /* 62: sampling activation control	 */
	unsigned int b63:1;	    /* 63: zero 			 */
	unsigned long interval;     /* 8-15: sampling interval		 */
	unsigned long tear;	    /* 16-23: TEAR contents		 */
	unsigned long dear;	    /* 24-31: DEAR contents		 */
	/* 32-63:							 */
	unsigned long rsvrd1;	    /* reserved 			 */
	unsigned long rsvrd2;	    /* reserved 			 */
	unsigned long rsvrd3;	    /* reserved 			 */
	unsigned long rsvrd4;	    /* reserved 			 */
} __packed;


struct hws_data_entry {
	unsigned int def:16;	    /* 0-15  Data Entry Format		 */
	unsigned int R:4;	    /* 16-19 reserved			 */
	unsigned int U:4;	    /* 20-23 Number of unique instruct.  */
	unsigned int z:2;	    /* zeros				 */
	unsigned int T:1;	    /* 26 PSW DAT mode			 */
	unsigned int W:1;	    /* 27 PSW wait state		 */
	unsigned int P:1;	    /* 28 PSW Problem state		 */
	unsigned int AS:2;	    /* 29-30 PSW address-space control	 */
	unsigned int I:1;	    /* 31 entry valid or invalid	 */
	unsigned int:16;
	unsigned int prim_asn:16;   /* primary ASN			 */
	unsigned long long ia;	    /* Instruction Address		 */
	unsigned long long gpp;     /* Guest Program Parameter		 */
	unsigned long long hpp;     /* Host Program Parameter		 */
} __packed;

struct hws_trailer_entry {
	unsigned int f:1;	    /* 0 - Block Full Indicator 	 */
	unsigned int a:1;	    /* 1 - Alert request control	 */
	unsigned int t:1;	    /* 2 - Timestamp format		 */
	unsigned long long:61;	    /* 3 - 63: Reserved 		 */
	unsigned long long overflow;	 /* 64 - sample Overflow count	      */
	unsigned long long timestamp;	 /* 16 - time-stamp		      */
	unsigned long long timestamp1;	 /*				      */
	unsigned long long reserved1;	 /* 32 -Reserved		      */
	unsigned long long reserved2;	 /*				      */
	unsigned long long progusage1;	 /* 48 - reserved for programming use */
	unsigned long long progusage2;	 /*				      */
} __packed;

/* Query counter information */
static inline int qctri(struct cpumf_ctr_info *info)
{
	int rc = -EINVAL;

	asm volatile (
		"0:	.insn	s,0xb28e0000,%1\n"
		"1:	lhi	%0,0\n"
		"2:\n"
		EX_TABLE(1b, 2b)
		: "+d" (rc), "=Q" (*info));
	return rc;
}

/* Load CPU-counter-set controls */
static inline int lcctl(u64 ctl)
{
	int cc;

	asm volatile (
		"	.insn	s,0xb2840000,%1\n"
		"	ipm	%0\n"
		"	srl	%0,28\n"
		: "=d" (cc) : "m" (ctl) : "cc");
	return cc;
}

/* Extract CPU counter */
static inline int ecctr(u64 ctr, u64 *val)
{
	register u64 content asm("4") = 0;
	int cc;

	asm volatile (
		"	.insn	rre,0xb2e40000,%0,%2\n"
		"	ipm	%1\n"
		"	srl	%1,28\n"
		: "=d" (content), "=d" (cc) : "d" (ctr) : "cc");
	if (!cc)
		*val = content;
	return cc;
}

/* Query sampling information */
static inline int qsi(struct hws_qsi_info_block *info)
{
	int cc;
	cc = 1;

	asm volatile(
		"0:	.insn	s,0xb2860000,0(%1)\n"
		"1:	lhi	%0,0\n"
		"2:\n"
		EX_TABLE(0b, 2b) EX_TABLE(1b, 2b)
		: "=d" (cc), "+a" (info)
		: "m" (*info)
		: "cc", "memory");

	return cc ? -EINVAL : 0;
}

/* Load sampling controls */
static inline int lsctl(struct hws_lsctl_request_block *req)
{
	int cc;

	cc = 1;
	asm volatile(
		"0:	.insn	s,0xb2870000,0(%1)\n"
		"1:	ipm	%0\n"
		"	srl	%0,28\n"
		"2:\n"
		EX_TABLE(0b, 2b) EX_TABLE(1b, 2b)
		: "+d" (cc), "+a" (req)
		: "m" (*req)
		: "cc", "memory");

	return cc ? -EINVAL : 0;
}

/* Sampling control helper functions */

#define SDB_TE_ALERT_REQ_MASK	0x4000000000000000UL
#define SDB_TE_BUFFER_FULL_MASK 0x8000000000000000UL

/* Return pointer to trailer entry of an sample data block */
static inline unsigned long *trailer_entry_ptr(unsigned long v)
{
	void *ret;

	ret = (void *) v;
	ret += PAGE_SIZE;
	ret -= sizeof(struct hws_trailer_entry);

	return (unsigned long *) ret;
}

/* Return if the entry in the sample data block table (sdbt)
 * is a link to the next sdbt */
static inline int is_link_entry(unsigned long *s)
{
	return *s & 0x1ul ? 1 : 0;
}

/* Return pointer to the linked sdbt */
static inline unsigned long *get_next_sdbt(unsigned long *s)
{
	return (unsigned long *) (*s & ~0x1ul);
}
#endif /* _ASM_S390_CPU_MF_H */
