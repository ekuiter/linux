// SPDX-License-Identifier: GPL-2.0
#include <linux/sched/task.h>
#include <linux/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/facility.h>
#include <asm/sections.h>
#include <asm/mem_detect.h>
#include "decompressor.h"
#include "boot.h"

#define init_mm			(*(struct mm_struct *)vmlinux.init_mm_off)
#define swapper_pg_dir		vmlinux.swapper_pg_dir_off
#define invalid_pg_dir		vmlinux.invalid_pg_dir_off

unsigned long __bootdata_preserved(s390_invalid_asce);
unsigned long __bootdata(pgalloc_pos);
unsigned long __bootdata(pgalloc_end);
unsigned long __bootdata(pgalloc_low);

enum populate_mode {
	POPULATE_ONE2ONE,
};

static void boot_check_oom(void)
{
	if (pgalloc_pos < pgalloc_low)
		error("out of memory on boot\n");
}

static void pgtable_populate_begin(unsigned long online_end)
{
	unsigned long initrd_end;
	unsigned long kernel_end;

	kernel_end = vmlinux.default_lma + vmlinux.image_size + vmlinux.bss_size;
	pgalloc_low = round_up(kernel_end, PAGE_SIZE);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD)) {
		initrd_end =  round_up(initrd_data.start + initrd_data.size, _SEGMENT_SIZE);
		pgalloc_low = max(pgalloc_low, initrd_end);
	}

	pgalloc_end = round_down(online_end, PAGE_SIZE);
	pgalloc_pos = pgalloc_end;

	boot_check_oom();
}

static void *boot_alloc_pages(unsigned int order)
{
	unsigned long size = PAGE_SIZE << order;

	pgalloc_pos -= size;
	pgalloc_pos = round_down(pgalloc_pos, size);

	boot_check_oom();

	return (void *)pgalloc_pos;
}

static void *boot_crst_alloc(unsigned long val)
{
	unsigned long *table;

	table = boot_alloc_pages(CRST_ALLOC_ORDER);
	if (table)
		crst_table_init(table, val);
	return table;
}

static pte_t *boot_pte_alloc(void)
{
	static void *pte_leftover;
	pte_t *pte;

	BUILD_BUG_ON(_PAGE_TABLE_SIZE * 2 != PAGE_SIZE);

	if (!pte_leftover) {
		pte_leftover = boot_alloc_pages(0);
		pte = pte_leftover + _PAGE_TABLE_SIZE;
	} else {
		pte = pte_leftover;
		pte_leftover = NULL;
	}
	memset64((u64 *)pte, _PAGE_INVALID, PTRS_PER_PTE);
	return pte;
}

static unsigned long _pa(unsigned long addr, enum populate_mode mode)
{
	switch (mode) {
	case POPULATE_ONE2ONE:
		return addr;
	default:
		return -1;
	}
}

static bool can_large_pud(pud_t *pu_dir, unsigned long addr, unsigned long end)
{
	return machine.has_edat2 &&
	       IS_ALIGNED(addr, PUD_SIZE) && (end - addr) >= PUD_SIZE;
}

static bool can_large_pmd(pmd_t *pm_dir, unsigned long addr, unsigned long end)
{
	return machine.has_edat1 &&
	       IS_ALIGNED(addr, PMD_SIZE) && (end - addr) >= PMD_SIZE;
}

static void pgtable_pte_populate(pmd_t *pmd, unsigned long addr, unsigned long end,
				 enum populate_mode mode)
{
	unsigned long next;
	pte_t *pte, entry;

	pte = pte_offset_kernel(pmd, addr);
	for (; addr < end; addr += PAGE_SIZE, pte++) {
		if (pte_none(*pte)) {
			entry = __pte(_pa(addr, mode));
			entry = set_pte_bit(entry, PAGE_KERNEL_EXEC);
			set_pte(pte, entry);
		}
	}
}

static void pgtable_pmd_populate(pud_t *pud, unsigned long addr, unsigned long end,
				 enum populate_mode mode)
{
	unsigned long next;
	pmd_t *pmd, entry;
	pte_t *pte;

	pmd = pmd_offset(pud, addr);
	for (; addr < end; addr = next, pmd++) {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd)) {
			if (can_large_pmd(pmd, addr, next)) {
				entry = __pmd(_pa(addr, mode));
				entry = set_pmd_bit(entry, SEGMENT_KERNEL_EXEC);
				set_pmd(pmd, entry);
				continue;
			}
			pte = boot_pte_alloc();
			pmd_populate(&init_mm, pmd, pte);
		} else if (pmd_large(*pmd)) {
			continue;
		}
		pgtable_pte_populate(pmd, addr, next, mode);
	}
}

static void pgtable_pud_populate(p4d_t *p4d, unsigned long addr, unsigned long end,
				 enum populate_mode mode)
{
	unsigned long next;
	pud_t *pud, entry;
	pmd_t *pmd;

	pud = pud_offset(p4d, addr);
	for (; addr < end; addr = next, pud++) {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud)) {
			if (can_large_pud(pud, addr, next)) {
				entry = __pud(_pa(addr, mode));
				entry = set_pud_bit(entry, REGION3_KERNEL_EXEC);
				set_pud(pud, entry);
				continue;
			}
			pmd = boot_crst_alloc(_SEGMENT_ENTRY_EMPTY);
			pud_populate(&init_mm, pud, pmd);
		} else if (pud_large(*pud)) {
			continue;
		}
		pgtable_pmd_populate(pud, addr, next, mode);
	}
}

static void pgtable_p4d_populate(pgd_t *pgd, unsigned long addr, unsigned long end,
				 enum populate_mode mode)
{
	unsigned long next;
	p4d_t *p4d;
	pud_t *pud;

	p4d = p4d_offset(pgd, addr);
	for (; addr < end; addr = next, p4d++) {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d)) {
			pud = boot_crst_alloc(_REGION3_ENTRY_EMPTY);
			p4d_populate(&init_mm, p4d, pud);
		}
		pgtable_pud_populate(p4d, addr, next, mode);
	}
}

static void pgtable_populate(unsigned long addr, unsigned long end, enum populate_mode mode)
{
	unsigned long next;
	pgd_t *pgd;
	p4d_t *p4d;

	pgd = pgd_offset(&init_mm, addr);
	for (; addr < end; addr = next, pgd++) {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd)) {
			p4d = boot_crst_alloc(_REGION2_ENTRY_EMPTY);
			pgd_populate(&init_mm, pgd, p4d);
		}
		pgtable_p4d_populate(pgd, addr, next, mode);
	}
}

/*
 * The pgtables are located in the range [pgalloc_pos, pgalloc_end).
 * That range must stay intact and is later reserved in the memblock.
 * Therefore pgtable_populate(pgalloc_pos, pgalloc_end) is needed to
 * finalize pgalloc_pos pointer. However that call can decrease the
 * value of pgalloc_pos pointer itself. Therefore, pgtable_populate()
 * needs to be called repeatedly until pgtables are complete and
 * pgalloc_pos does not grow left anymore.
 */
static void pgtable_populate_end(void)
{
	unsigned long pgalloc_end_curr = pgalloc_end;
	unsigned long pgalloc_pos_prev;

	do {
		pgalloc_pos_prev = pgalloc_pos;
		pgtable_populate(pgalloc_pos, pgalloc_end_curr, POPULATE_ONE2ONE);
		pgalloc_end_curr = pgalloc_pos_prev;
	} while (pgalloc_pos < pgalloc_pos_prev);
}

void setup_vmem(unsigned long online_end, unsigned long asce_limit)
{
	unsigned long asce_type;
	unsigned long asce_bits;

	if (asce_limit == _REGION1_SIZE) {
		asce_type = _REGION2_ENTRY_EMPTY;
		asce_bits = _ASCE_TYPE_REGION2 | _ASCE_TABLE_LENGTH;
	} else {
		asce_type = _REGION3_ENTRY_EMPTY;
		asce_bits = _ASCE_TYPE_REGION3 | _ASCE_TABLE_LENGTH;
	}
	s390_invalid_asce = invalid_pg_dir | _ASCE_TYPE_REGION3 | _ASCE_TABLE_LENGTH;

	crst_table_init((unsigned long *)swapper_pg_dir, asce_type);
	crst_table_init((unsigned long *)invalid_pg_dir, _REGION3_ENTRY_EMPTY);

	/*
	 * To allow prefixing the lowcore must be mapped with 4KB pages.
	 * To prevent creation of a large page at address 0 first map
	 * the lowcore and create the identity mapping only afterwards.
	 *
	 * No further pgtable_populate() calls are allowed after the value
	 * of pgalloc_pos finalized with a call to pgtable_populate_end().
	 */
	pgtable_populate_begin(online_end);
	pgtable_populate(0, sizeof(struct lowcore), POPULATE_ONE2ONE);
	pgtable_populate(0, online_end, POPULATE_ONE2ONE);
	pgtable_populate_end();

	S390_lowcore.kernel_asce = swapper_pg_dir | asce_bits;
	S390_lowcore.user_asce = s390_invalid_asce;

	__ctl_load(S390_lowcore.kernel_asce, 1, 1);
	__ctl_load(S390_lowcore.user_asce, 7, 7);
	__ctl_load(S390_lowcore.kernel_asce, 13, 13);

	init_mm.context.asce = S390_lowcore.kernel_asce;
}
