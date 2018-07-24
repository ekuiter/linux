// SPDX-License-Identifier: GPL-2.0
/*
 * Implementation of Gasket page table support.
 *
 * Copyright (C) 2018 Google, Inc.
 */

/*
 * Implementation of Gasket page table support.
 *
 * This file assumes 4kB pages throughout; can be factored out when necessary.
 *
 * Address format is as follows:
 * Simple addresses - those whose containing pages are directly placed in the
 * device's address translation registers - are laid out as:
 * [ 63 - 40: Unused | 39 - 28: 0 | 27 - 12: page index | 11 - 0: page offset ]
 * page index:  The index of the containing page in the device's address
 *              translation registers.
 * page offset: The index of the address into the containing page.
 *
 * Extended address - those whose containing pages are contained in a second-
 * level page table whose address is present in the device's address translation
 * registers - are laid out as:
 * [ 63 - 40: Unused | 39: flag | 38 - 37: 0 | 36 - 21: dev/level 0 index |
 *   20 - 12: host/level 1 index | 11 - 0: page offset ]
 * flag:        Marker indicating that this is an extended address. Always 1.
 * dev index:   The index of the first-level page in the device's extended
 *              address translation registers.
 * host index:  The index of the containing page in the [host-resident] second-
 *              level page table.
 * page offset: The index of the address into the containing [second-level]
 *              page.
 */
#include "gasket_page_table.h"

#include <linux/file.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>

#include "gasket_constants.h"
#include "gasket_core.h"
#include "gasket_logging.h"

/* Constants & utility macros */
/* The number of pages that can be mapped into each second-level page table. */
#define GASKET_PAGES_PER_SUBTABLE 512

/* The starting position of the page index in a simple virtual address. */
#define GASKET_SIMPLE_PAGE_SHIFT 12

/* Flag indicating that a [device] slot is valid for use. */
#define GASKET_VALID_SLOT_FLAG 1

/*
 * The starting position of the level 0 page index (i.e., the entry in the
 * device's extended address registers) in an extended address.
 * Also can be thought of as (log2(PAGE_SIZE) + log2(PAGES_PER_SUBTABLE)),
 * or (12 + 9).
 */
#define GASKET_EXTENDED_LVL0_SHIFT 21

/*
 * Number of first level pages that Gasket chips support. Equivalent to
 * log2(NUM_LVL0_PAGE_TABLES)
 *
 * At a maximum, allowing for a 34 bits address space (or 16GB)
 *   = GASKET_EXTENDED_LVL0_WIDTH + (log2(PAGE_SIZE) + log2(PAGES_PER_SUBTABLE)
 * or, = 13 + 9 + 12
 */
#define GASKET_EXTENDED_LVL0_WIDTH 13

/*
 * The starting position of the level 1 page index (i.e., the entry in the
 * host second-level/sub- table) in an extended address.
 */
#define GASKET_EXTENDED_LVL1_SHIFT 12

/* Page-table specific error logging. */
#define gasket_pg_tbl_error(pg_tbl, format, arg...)                            \
	gasket_dev_log(err, (pg_tbl)->device, (struct pci_dev *)NULL, format,  \
		##arg)

/* Type declarations */
/* Valid states for a struct gasket_page_table_entry. */
enum pte_status {
	PTE_FREE,
	PTE_INUSE,
};

/*
 * Mapping metadata for a single page.
 *
 * In this file, host-side page table entries are referred to as that (or PTEs).
 * Where device vs. host entries are differentiated, device-side or -visible
 * entries are called "slots". A slot may be either an entry in the device's
 * address translation table registers or an entry in a second-level page
 * table ("subtable").
 *
 * The full data in this structure is visible on the host [of course]. Only
 * the address contained in dma_addr is communicated to the device; that points
 * to the actual page mapped and described by this structure.
 */
struct gasket_page_table_entry {
	/* The status of this entry/slot: free or in use. */
	enum pte_status status;

	/* Address of the page in DMA space. */
	dma_addr_t dma_addr;

	/* Linux page descriptor for the page described by this structure. */
	struct page *page;

	/*
	 * Index for alignment into host vaddrs.
	 * When a user specifies a host address for a mapping, that address may
	 * not be page-aligned. Offset is the index into the containing page of
	 * the host address (i.e., host_vaddr & (PAGE_SIZE - 1)).
	 * This is necessary for translating between user-specified addresses
	 * and page-aligned addresses.
	 */
	int offset;

	/*
	 * If this is an extended and first-level entry, sublevel points
	 * to the second-level entries underneath this entry.
	 */
	struct gasket_page_table_entry *sublevel;
};

/*
 * Maintains virtual to physical address mapping for a coherent page that is
 * allocated by this module for a given device.
 * Note that coherent pages mappings virt mapping cannot be tracked by the
 * Linux kernel, and coherent pages don't have a struct page associated,
 * hence Linux kernel cannot perform a get_user_page_xx() on a phys address
 * that was allocated coherent.
 * This structure trivially implements this mechanism.
 */
struct gasket_coherent_page_entry {
	/* Phys address, dma'able by the owner device */
	dma_addr_t paddr;

	/* Kernel virtual address */
	u64 user_virt;

	/* User virtual address that was mapped by the mmap kernel subsystem */
	u64 kernel_virt;

	/*
	 * Whether this page has been mapped into a user land process virtual
	 * space
	 */
	u32 in_use;
};

/*
 * [Host-side] page table descriptor.
 *
 * This structure tracks the metadata necessary to manage both simple and
 * extended page tables.
 */
struct gasket_page_table {
	/* The config used to create this page table. */
	struct gasket_page_table_config config;

	/* The number of simple (single-level) entries in the page table. */
	uint num_simple_entries;

	/* The number of extended (two-level) entries in the page table. */
	uint num_extended_entries;

	/* Array of [host-side] page table entries. */
	struct gasket_page_table_entry *entries;

	/* Number of actively mapped kernel pages in this table. */
	uint num_active_pages;

	/* Device register: base of/first slot in the page table. */
	u64 __iomem *base_slot;

	/* Device register: holds the offset indicating the start of the
	 * extended address region of the device's address translation table.
	 */
	u64 __iomem *extended_offset_reg;

	/* Device structure for the underlying device. Only used for logging. */
	struct device *device;

	/* PCI system descriptor for the underlying device. */
	struct pci_dev *pci_dev;

	/* Location of the extended address bit for this Gasket device. */
	u64 extended_flag;

	/* Mutex to protect page table internals. */
	struct mutex mutex;

	/* Number of coherent pages accessible thru by this page table */
	int num_coherent_pages;

	/*
	 * List of coherent memory (physical) allocated for a device.
	 *
	 * This structure also remembers the user virtual mapping, this is
	 * hacky, but we need to do this because the kernel doesn't keep track
	 * of the user coherent pages (pfn pages), and virt to coherent page
	 * mapping.
	 * TODO: use find_vma() APIs to convert host address to vm_area, to
	 * dma_addr_t instead of storing user virtu address in
	 * gasket_coherent_page_entry
	 *
	 * Note that the user virtual mapping is created by the driver, in
	 * gasket_mmap function, so user_virt belongs in the driver anyhow.
	 */
	struct gasket_coherent_page_entry *coherent_pages;

	/*
	 * Whether the page table uses arch specific dma_ops or
	 * whether the driver is supplying its own.
	 */
	bool dma_ops;
};

/* Mapping declarations */
static int gasket_map_simple_pages(
	struct gasket_page_table *pg_tbl, ulong host_addr,
	ulong dev_addr, uint num_pages);
static int gasket_map_extended_pages(
	struct gasket_page_table *pg_tbl, ulong host_addr,
	ulong dev_addr, uint num_pages);
static int gasket_perform_mapping(
	struct gasket_page_table *pg_tbl,
	struct gasket_page_table_entry *pte_base, u64 __iomem *att_base,
	ulong host_addr, uint num_pages, int is_simple_mapping);

static int gasket_alloc_simple_entries(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages);
static int gasket_alloc_extended_entries(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_entries);
static int gasket_alloc_extended_subtable(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *pte,
	u64 __iomem *att_reg);

/* Unmapping declarations */
static void gasket_page_table_unmap_nolock(
	struct gasket_page_table *pg_tbl, ulong start_addr, uint num_pages);
static void gasket_page_table_unmap_all_nolock(
	struct gasket_page_table *pg_tbl);
static void gasket_unmap_simple_pages(
	struct gasket_page_table *pg_tbl, ulong start_addr, uint num_pages);
static void gasket_unmap_extended_pages(
	struct gasket_page_table *pg_tbl, ulong start_addr, uint num_pages);
static void gasket_perform_unmapping(
	struct gasket_page_table *pg_tbl,
	struct gasket_page_table_entry *pte_base, u64 __iomem *att_base,
	uint num_pages, int is_simple_mapping);

static void gasket_free_extended_subtable(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *pte,
	u64 __iomem *att_reg);
static bool gasket_release_page(struct page *page);

/* Other/utility declarations */
static inline bool gasket_addr_is_simple(
	struct gasket_page_table *pg_tbl, ulong addr);
static bool gasket_is_simple_dev_addr_bad(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages);
static bool gasket_is_extended_dev_addr_bad(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages);
static bool gasket_is_pte_range_free(
	struct gasket_page_table_entry *pte, uint num_entries);
static void gasket_page_table_garbage_collect_nolock(
	struct gasket_page_table *pg_tbl);

/* Address format declarations */
static ulong gasket_components_to_dev_address(
	struct gasket_page_table *pg_tbl, int is_simple, uint page_index,
	uint offset);
static int gasket_simple_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr);
static ulong gasket_extended_lvl0_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr);
static ulong gasket_extended_lvl1_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr);

static int is_coherent(struct gasket_page_table *pg_tbl, ulong host_addr);

/* Public/exported functions */
/* See gasket_page_table.h for description. */
int gasket_page_table_init(
	struct gasket_page_table **ppg_tbl,
	const struct gasket_bar_data *bar_data,
	const struct gasket_page_table_config *page_table_config,
	struct device *device, struct pci_dev *pci_dev, bool has_dma_ops)
{
	ulong bytes;
	struct gasket_page_table *pg_tbl;
	ulong total_entries = page_table_config->total_entries;

	/*
	 * TODO: Verify config->total_entries against value read from the
	 * hardware register that contains the page table size.
	 */
	if (total_entries == ULONG_MAX) {
		gasket_nodev_debug(
			"Error reading page table size. "
			"Initializing page table with size 0.");
		total_entries = 0;
	}

	gasket_nodev_debug(
		"Attempting to initialize page table of size 0x%lx.",
		total_entries);

	gasket_nodev_debug(
		"Table has base reg 0x%x, extended offset reg 0x%x.",
		page_table_config->base_reg,
		page_table_config->extended_reg);

	*ppg_tbl = kzalloc(sizeof(**ppg_tbl), GFP_KERNEL);
	if (!*ppg_tbl) {
		gasket_nodev_debug("No memory for page table.");
		return -ENOMEM;
	}

	pg_tbl = *ppg_tbl;
	bytes = total_entries * sizeof(struct gasket_page_table_entry);
	if (bytes != 0) {
		pg_tbl->entries = vmalloc(bytes);
		if (!pg_tbl->entries) {
			gasket_nodev_debug(
				"No memory for address translation metadata.");
			kfree(pg_tbl);
			*ppg_tbl = NULL;
			return -ENOMEM;
		}
		memset(pg_tbl->entries, 0, bytes);
	}

	mutex_init(&pg_tbl->mutex);
	memcpy(&pg_tbl->config, page_table_config, sizeof(*page_table_config));
	if (pg_tbl->config.mode == GASKET_PAGE_TABLE_MODE_NORMAL ||
	    pg_tbl->config.mode == GASKET_PAGE_TABLE_MODE_SIMPLE) {
		pg_tbl->num_simple_entries = total_entries;
		pg_tbl->num_extended_entries = 0;
		pg_tbl->extended_flag = 1ull << page_table_config->extended_bit;
	} else {
		pg_tbl->num_simple_entries = 0;
		pg_tbl->num_extended_entries = total_entries;
		pg_tbl->extended_flag = 0;
	}
	pg_tbl->num_active_pages = 0;
	pg_tbl->base_slot = (u64 __iomem *)&(
		bar_data->virt_base[page_table_config->base_reg]);
	pg_tbl->extended_offset_reg = (u64 __iomem *)&(
		bar_data->virt_base[page_table_config->extended_reg]);
	pg_tbl->device = device;
	pg_tbl->pci_dev = pci_dev;
	pg_tbl->dma_ops = has_dma_ops;

	gasket_nodev_debug("Page table initialized successfully.");

	return 0;
}

/* See gasket_page_table.h for description. */
void gasket_page_table_cleanup(struct gasket_page_table *pg_tbl)
{
	/* Deallocate free second-level tables. */
	gasket_page_table_garbage_collect(pg_tbl);

	/* TODO: Check that all PTEs have been freed? */

	vfree(pg_tbl->entries);
	pg_tbl->entries = NULL;

	kfree(pg_tbl);
}

/* See gasket_page_table.h for description. */
int gasket_page_table_partition(
	struct gasket_page_table *pg_tbl, uint num_simple_entries)
{
	int i, start;

	mutex_lock(&pg_tbl->mutex);
	if (num_simple_entries > pg_tbl->config.total_entries) {
		mutex_unlock(&pg_tbl->mutex);
		return -EINVAL;
	}

	gasket_page_table_garbage_collect_nolock(pg_tbl);

	start = min(pg_tbl->num_simple_entries, num_simple_entries);

	for (i = start; i < pg_tbl->config.total_entries; i++) {
		if (pg_tbl->entries[i].status != PTE_FREE) {
			gasket_pg_tbl_error(pg_tbl, "entry %d is not free", i);
			mutex_unlock(&pg_tbl->mutex);
			return -EBUSY;
		}
	}

	pg_tbl->num_simple_entries = num_simple_entries;
	pg_tbl->num_extended_entries =
		pg_tbl->config.total_entries - num_simple_entries;
	writeq(num_simple_entries, pg_tbl->extended_offset_reg);

	mutex_unlock(&pg_tbl->mutex);
	return 0;
}
EXPORT_SYMBOL(gasket_page_table_partition);

/*
 * See gasket_page_table.h for general description.
 *
 * gasket_page_table_map calls either gasket_map_simple_pages() or
 * gasket_map_extended_pages() to actually perform the mapping.
 *
 * The page table mutex is held for the entire operation.
 */
int gasket_page_table_map(
	struct gasket_page_table *pg_tbl, ulong host_addr, ulong dev_addr,
	uint num_pages)
{
	int ret;

	if (!num_pages)
		return 0;

	mutex_lock(&pg_tbl->mutex);

	if (gasket_addr_is_simple(pg_tbl, dev_addr)) {
		ret = gasket_map_simple_pages(
			pg_tbl, host_addr, dev_addr, num_pages);
	} else {
		ret = gasket_map_extended_pages(
			pg_tbl, host_addr, dev_addr, num_pages);
	}

	mutex_unlock(&pg_tbl->mutex);

	gasket_nodev_debug(
		"%s done: ha %llx daddr %llx num %d, "
		"ret %d\n",
		__func__,
		(unsigned long long)host_addr,
		(unsigned long long)dev_addr, num_pages, ret);
	return ret;
}
EXPORT_SYMBOL(gasket_page_table_map);

/*
 * See gasket_page_table.h for general description.
 *
 * gasket_page_table_unmap takes the page table lock and calls either
 * gasket_unmap_simple_pages() or gasket_unmap_extended_pages() to
 * actually unmap the pages from device space.
 *
 * The page table mutex is held for the entire operation.
 */
void gasket_page_table_unmap(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	if (!num_pages)
		return;

	mutex_lock(&pg_tbl->mutex);
	gasket_page_table_unmap_nolock(pg_tbl, dev_addr, num_pages);
	mutex_unlock(&pg_tbl->mutex);
}
EXPORT_SYMBOL(gasket_page_table_unmap);

static void gasket_page_table_unmap_all_nolock(struct gasket_page_table *pg_tbl)
{
	gasket_unmap_simple_pages(
		pg_tbl, gasket_components_to_dev_address(pg_tbl, 1, 0, 0),
		pg_tbl->num_simple_entries);
	gasket_unmap_extended_pages(
		pg_tbl, gasket_components_to_dev_address(pg_tbl, 0, 0, 0),
		pg_tbl->num_extended_entries * GASKET_PAGES_PER_SUBTABLE);
}

/* See gasket_page_table.h for description. */
void gasket_page_table_unmap_all(struct gasket_page_table *pg_tbl)
{
	mutex_lock(&pg_tbl->mutex);
	gasket_page_table_unmap_all_nolock(pg_tbl);
	mutex_unlock(&pg_tbl->mutex);
}
EXPORT_SYMBOL(gasket_page_table_unmap_all);

/* See gasket_page_table.h for description. */
void gasket_page_table_reset(struct gasket_page_table *pg_tbl)
{
	mutex_lock(&pg_tbl->mutex);
	gasket_page_table_unmap_all_nolock(pg_tbl);
	writeq(pg_tbl->config.total_entries, pg_tbl->extended_offset_reg);
	mutex_unlock(&pg_tbl->mutex);
}

/* See gasket_page_table.h for description. */
void gasket_page_table_garbage_collect(struct gasket_page_table *pg_tbl)
{
	mutex_lock(&pg_tbl->mutex);
	gasket_page_table_garbage_collect_nolock(pg_tbl);
	mutex_unlock(&pg_tbl->mutex);
}

/* See gasket_page_table.h for description. */
int gasket_page_table_lookup_page(
	struct gasket_page_table *pg_tbl, ulong dev_addr, struct page **ppage,
	ulong *poffset)
{
	uint page_num;
	struct gasket_page_table_entry *pte;

	mutex_lock(&pg_tbl->mutex);
	if (gasket_addr_is_simple(pg_tbl, dev_addr)) {
		page_num = gasket_simple_page_idx(pg_tbl, dev_addr);
		if (page_num >= pg_tbl->num_simple_entries)
			goto fail;

		pte = pg_tbl->entries + page_num;
		if (pte->status != PTE_INUSE)
			goto fail;
	} else {
		/* Find the level 0 entry, */
		page_num = gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);
		if (page_num >= pg_tbl->num_extended_entries)
			goto fail;

		pte = pg_tbl->entries + pg_tbl->num_simple_entries + page_num;
		if (pte->status != PTE_INUSE)
			goto fail;

		/* and its contained level 1 entry. */
		page_num = gasket_extended_lvl1_page_idx(pg_tbl, dev_addr);
		pte = pte->sublevel + page_num;
		if (pte->status != PTE_INUSE)
			goto fail;
	}

	*ppage = pte->page;
	*poffset = pte->offset;
	mutex_unlock(&pg_tbl->mutex);
	return 0;

fail:
	*ppage = NULL;
	*poffset = 0;
	mutex_unlock(&pg_tbl->mutex);
	return -1;
}

/* See gasket_page_table.h for description. */
bool gasket_page_table_are_addrs_bad(
	struct gasket_page_table *pg_tbl, ulong host_addr, ulong dev_addr,
	ulong bytes)
{
	if (host_addr & (PAGE_SIZE - 1)) {
		gasket_pg_tbl_error(
			pg_tbl,
			"host mapping address 0x%lx must be page aligned",
			host_addr);
		return true;
	}

	return gasket_page_table_is_dev_addr_bad(pg_tbl, dev_addr, bytes);
}
EXPORT_SYMBOL(gasket_page_table_are_addrs_bad);

/* See gasket_page_table.h for description. */
bool gasket_page_table_is_dev_addr_bad(
	struct gasket_page_table *pg_tbl, ulong dev_addr, ulong bytes)
{
	uint num_pages = bytes / PAGE_SIZE;

	if (bytes & (PAGE_SIZE - 1)) {
		gasket_pg_tbl_error(
			pg_tbl,
			"mapping size 0x%lX must be page aligned", bytes);
		return true;
	}

	if (num_pages == 0) {
		gasket_pg_tbl_error(
			pg_tbl,
			"requested mapping is less than one page: %lu / %lu",
			bytes, PAGE_SIZE);
		return true;
	}

	if (gasket_addr_is_simple(pg_tbl, dev_addr))
		return gasket_is_simple_dev_addr_bad(
			pg_tbl, dev_addr, num_pages);
	return gasket_is_extended_dev_addr_bad(pg_tbl, dev_addr, num_pages);
}
EXPORT_SYMBOL(gasket_page_table_is_dev_addr_bad);

/* See gasket_page_table.h for description. */
uint gasket_page_table_max_size(struct gasket_page_table *page_table)
{
	if (!page_table) {
		gasket_nodev_error("Passed a null page table.");
		return 0;
	}
	return page_table->config.total_entries;
}
EXPORT_SYMBOL(gasket_page_table_max_size);

/* See gasket_page_table.h for description. */
uint gasket_page_table_num_entries(struct gasket_page_table *pg_tbl)
{
	if (!pg_tbl) {
		gasket_nodev_error("Passed a null page table.");
		return 0;
	}

	return pg_tbl->num_simple_entries + pg_tbl->num_extended_entries;
}
EXPORT_SYMBOL(gasket_page_table_num_entries);

/* See gasket_page_table.h for description. */
uint gasket_page_table_num_simple_entries(struct gasket_page_table *pg_tbl)
{
	if (!pg_tbl) {
		gasket_nodev_error("Passed a null page table.");
		return 0;
	}

	return pg_tbl->num_simple_entries;
}
EXPORT_SYMBOL(gasket_page_table_num_simple_entries);

/* See gasket_page_table.h for description. */
uint gasket_page_table_num_active_pages(struct gasket_page_table *pg_tbl)
{
	if (!pg_tbl) {
		gasket_nodev_error("Passed a null page table.");
		return 0;
	}

	return pg_tbl->num_active_pages;
}
EXPORT_SYMBOL(gasket_page_table_num_active_pages);

/* See gasket_page_table.h */
int gasket_page_table_system_status(struct gasket_page_table *page_table)
{
	if (!page_table) {
		gasket_nodev_error("Passed a null page table.");
		return GASKET_STATUS_LAMED;
	}

	if (gasket_page_table_num_entries(page_table) == 0) {
		gasket_nodev_debug("Page table size is 0.");
		return GASKET_STATUS_LAMED;
	}

	return GASKET_STATUS_ALIVE;
}

/* Internal functions */

/* Mapping functions */
/*
 * Allocate and map pages to simple addresses.
 * @pg_tbl: Gasket page table pointer.
 * @host_addr: Starting host virtual memory address of the pages.
 * @dev_addr: Starting device address of the pages.
 * @cnt: Count of the number of device pages to map.
 *
 * Description: gasket_map_simple_pages calls gasket_simple_alloc_pages() to
 *		allocate the page table slots, then calls
 *		gasket_perform_mapping() to actually do the work of mapping the
 *		pages into the the simple page table (device translation table
 *		registers).
 *
 *		The sd_mutex must be held when gasket_map_simple_pages() is
 *		called.
 *
 *		Returns 0 if successful or a non-zero error number otherwise.
 *		If there is an error, no pages are mapped.
 */
static int gasket_map_simple_pages(
	struct gasket_page_table *pg_tbl, ulong host_addr, ulong dev_addr,
	uint num_pages)
{
	int ret;
	uint slot_idx = gasket_simple_page_idx(pg_tbl, dev_addr);

	ret = gasket_alloc_simple_entries(pg_tbl, dev_addr, num_pages);
	if (ret) {
		gasket_pg_tbl_error(
			pg_tbl,
			"page table slots %u (@ 0x%lx) to %u are not available",
			slot_idx, dev_addr, slot_idx + num_pages - 1);
		return ret;
	}

	ret = gasket_perform_mapping(
		pg_tbl, pg_tbl->entries + slot_idx,
		pg_tbl->base_slot + slot_idx, host_addr, num_pages, 1);

	if (ret) {
		gasket_page_table_unmap_nolock(pg_tbl, dev_addr, num_pages);
		gasket_pg_tbl_error(pg_tbl, "gasket_perform_mapping %d.", ret);
	}
	return ret;
}

/*
 * gasket_map_extended_pages - Get and map buffers to extended addresses.
 * @pg_tbl: Gasket page table pointer.
 * @host_addr: Starting host virtual memory address of the pages.
 * @dev_addr: Starting device address of the pages.
 * @num_pages: The number of device pages to map.
 *
 * Description: gasket_map_extended_buffers calls
 *		gasket_alloc_extended_entries() to allocate the page table
 *		slots, then loops over the level 0 page table entries, and for
 *		each calls gasket_perform_mapping() to map the buffers into the
 *		level 1 page table for that level 0 entry.
 *
 *		The page table mutex must be held when
 *		gasket_map_extended_pages() is called.
 *
 *		Returns 0 if successful or a non-zero error number otherwise.
 *		If there is an error, no pages are mapped.
 */
static int gasket_map_extended_pages(
	struct gasket_page_table *pg_tbl, ulong host_addr, ulong dev_addr,
	uint num_pages)
{
	int ret;
	ulong dev_addr_end;
	uint slot_idx, remain, len;
	struct gasket_page_table_entry *pte;
	u64 __iomem *slot_base;

	ret = gasket_alloc_extended_entries(pg_tbl, dev_addr, num_pages);
	if (ret) {
		dev_addr_end = dev_addr + (num_pages / PAGE_SIZE) - 1;
		gasket_pg_tbl_error(
			pg_tbl,
			"page table slots (%lu,%lu) (@ 0x%lx) to (%lu,%lu) are "
			"not available",
			gasket_extended_lvl0_page_idx(pg_tbl, dev_addr),
			dev_addr,
			gasket_extended_lvl1_page_idx(pg_tbl, dev_addr),
			gasket_extended_lvl0_page_idx(pg_tbl, dev_addr_end),
			gasket_extended_lvl1_page_idx(pg_tbl, dev_addr_end));
		return ret;
	}

	remain = num_pages;
	slot_idx = gasket_extended_lvl1_page_idx(pg_tbl, dev_addr);
	pte = pg_tbl->entries + pg_tbl->num_simple_entries +
	      gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);

	while (remain > 0) {
		len = min(remain, GASKET_PAGES_PER_SUBTABLE - slot_idx);

		slot_base =
			(u64 __iomem *)(page_address(pte->page) + pte->offset);
		ret = gasket_perform_mapping(
			pg_tbl, pte->sublevel + slot_idx, slot_base + slot_idx,
			host_addr, len, 0);
		if (ret) {
			gasket_page_table_unmap_nolock(
				pg_tbl, dev_addr, num_pages);
			return ret;
		}

		remain -= len;
		slot_idx = 0;
		pte++;
		host_addr += len * PAGE_SIZE;
	}

	return 0;
}

/*
 * TODO: dma_map_page() is not plugged properly when running under qemu. i.e.
 * dma_ops are not set properly, which causes the kernel to assert.
 *
 * This temporary hack allows the driver to work on qemu, but need to be fixed:
 * - either manually set the dma_ops for the architecture (which incidentally
 * can't be done in an out-of-tree module) - or get qemu to fill the device tree
 * properly so as linux plug the proper dma_ops or so as the driver can detect
 * that it is runnig on qemu
 */
static inline dma_addr_t _no_op_dma_map_page(
	struct device *dev, struct page *page, size_t offset, size_t size,
	enum dma_data_direction dir)
{
	/*
	 * struct dma_map_ops *ops = get_dma_ops(dev);
	 * dma_addr_t addr;
	 *
	 * kmemcheck_mark_initialized(page_address(page) + offset, size);
	 * BUG_ON(!valid_dma_direction(dir));
	 * addr = ops->map_page(dev, page, offset, size, dir, NULL);
	 * debug_dma_map_page(dev, page, offset, size, dir, addr, false);
	 */

	return page_to_phys(page);
}

/*
 * Get and map last level page table buffers.
 * @pg_tbl: Gasket page table pointer.
 * @ptes: Array of page table entries to describe this mapping, one per
 *        page to map.
 * @slots: Location(s) to write device-mapped page address. If this is a simple
 *	   mapping, these will be address translation registers. If this is
 *	   an extended mapping, these will be within a second-level page table
 *	   allocated by the host and so must have their __iomem attribute
 *	   casted away.
 * @host_addr: Starting [host] virtual memory address of the buffers.
 * @num_pages: The number of device pages to map.
 * @is_simple_mapping: 1 if this is a simple mapping, 0 otherwise.
 *
 * Description: gasket_perform_mapping calls get_user_pages() to get pages
 *		of user memory and pin them.  It then calls dma_map_page() to
 *		map them for DMA.  Finally, the mapped DMA addresses are written
 *		into the page table.
 *
 *		This function expects that the page table entries are
 *		already allocated.  The level argument determines how the
 *		final page table entries are written: either into PCIe memory
 *		mapped space for a level 0 page table or into kernel memory
 *		for a level 1 page table.
 *
 *		The page pointers are saved for later releasing the pages.
 *
 *		Returns 0 if successful or a non-zero error number otherwise.
 */
static int gasket_perform_mapping(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *ptes,
	u64 __iomem *slots, ulong host_addr, uint num_pages,
	int is_simple_mapping)
{
	int ret;
	ulong offset;
	struct page *page;
	dma_addr_t dma_addr;
	ulong page_addr;
	int i;

	for (i = 0; i < num_pages; i++) {
		page_addr = host_addr + i * PAGE_SIZE;
		offset = page_addr & (PAGE_SIZE - 1);
		gasket_nodev_debug("%s i %d\n", __func__, i);
		if (is_coherent(pg_tbl, host_addr)) {
			u64 off =
				(u64)host_addr -
				(u64)pg_tbl->coherent_pages[0].user_virt;
			ptes[i].page = NULL;
			ptes[i].offset = offset;
			ptes[i].dma_addr = pg_tbl->coherent_pages[0].paddr +
					   off + i * PAGE_SIZE;
		} else {
			ret = get_user_pages_fast(
				page_addr - offset, 1, 1, &page);

			if (ret <= 0) {
				gasket_pg_tbl_error(
					pg_tbl,
					"get user pages failed for addr=0x%lx, "
					"offset=0x%lx [ret=%d]",
					page_addr, offset, ret);
				return ret ? ret : -ENOMEM;
			}
			++pg_tbl->num_active_pages;

			ptes[i].page = page;
			ptes[i].offset = offset;

			/* Map the page into DMA space. */
			if (pg_tbl->dma_ops) {
				/* hook in to kernel map functions */
				ptes[i].dma_addr = dma_map_page(pg_tbl->device,
					page, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
			} else {
				ptes[i].dma_addr = _no_op_dma_map_page(
					pg_tbl->device, page, 0, PAGE_SIZE,
					DMA_BIDIRECTIONAL);
			}

			gasket_nodev_debug(
				"%s dev %p "
				"i %d pte %p pfn %p -> mapped %llx\n",
				__func__,
				pg_tbl->device, i, &ptes[i],
				(void *)page_to_pfn(page),
				(unsigned long long)ptes[i].dma_addr);

			if (ptes[i].dma_addr == -1) {
				gasket_nodev_debug(
					"%s i %d"
					" -> fail to map page %llx "
					"[pfn %p ohys %p]\n",
					__func__,
					i,
					(unsigned long long)ptes[i].dma_addr,
					(void *)page_to_pfn(page),
					(void *)page_to_phys(page));
				return -1;
			}
			/* Wait until the page is mapped. */
			mb();
		}

		/* Make the DMA-space address available to the device. */
		dma_addr = (ptes[i].dma_addr + offset) | GASKET_VALID_SLOT_FLAG;

		if (is_simple_mapping) {
			writeq(dma_addr, &slots[i]);
		} else {
			((u64 __force *)slots)[i] = dma_addr;
			/* Extended page table vectors are in DRAM,
			 * and so need to be synced each time they are updated.
			 */
			dma_map_single(pg_tbl->device,
				       (void *)&((u64 __force *)slots)[i],
				       sizeof(u64), DMA_TO_DEVICE);
		}
		ptes[i].status = PTE_INUSE;
	}
	return 0;
}

/**
 * Allocate page table entries in a simple table.
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: Starting device address for the (eventual) mappings.
 * @num_pages: Count of pages to be mapped.
 *
 * Description: gasket_alloc_simple_entries checks to see if a range of page
 *		table slots are available.  As long as the sd_mutex is
 *		held, the slots will be available.
 *
 *		The page table mutex must be held when
 *		gasket_alloc_simple entries() is called.
 *
 *		Returns 0 if successful, or non-zero if the requested device
 *		addresses are not available.
 */
static int gasket_alloc_simple_entries(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	if (!gasket_is_pte_range_free(
		    pg_tbl->entries + gasket_simple_page_idx(pg_tbl, dev_addr),
		    num_pages))
		return -EBUSY;

	return 0;
}

/**
 * Allocate slots in an extended page table.
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: Starting device address for the (eventual) mappings.
 * @num_pages: Count of pages to be mapped.
 *
 * Description: gasket_alloc_extended_entries checks to see if a range of page
 *		table slots are available. If necessary, memory is allocated for
 *		second level page tables.
 *
 *		Note that memory for second level page tables is allocated
 *		as needed, but that memory is only freed on the final close
 *		of the device file, when the page tables are repartitioned,
 *		or the the device is removed.  If there is an error or if
 *		the full range of slots is not available, any memory
 *		allocated for second level page tables remains allocated
 *		until final close, repartition, or device removal.
 *
 *		The page table mutex must be held when
 *		gasket_alloc_extended_entries() is called.
 *
 *		Returns 0 if successful, or non-zero if the slots are
 *		not available.
 */
static int gasket_alloc_extended_entries(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_entries)
{
	int ret = 0;
	uint remain, subtable_slot_idx, len;
	struct gasket_page_table_entry *pte;
	u64 __iomem *slot;

	remain = num_entries;
	subtable_slot_idx = gasket_extended_lvl1_page_idx(pg_tbl, dev_addr);
	pte = pg_tbl->entries + pg_tbl->num_simple_entries +
	      gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);
	slot = pg_tbl->base_slot + pg_tbl->num_simple_entries +
	       gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);

	while (remain > 0) {
		len = min(remain,
			  GASKET_PAGES_PER_SUBTABLE - subtable_slot_idx);

		if (pte->status == PTE_FREE) {
			ret = gasket_alloc_extended_subtable(pg_tbl, pte, slot);
			if (ret) {
				gasket_pg_tbl_error(
					pg_tbl,
					"no memory for extended addr subtable");
				return ret;
			}
		} else {
			if (!gasket_is_pte_range_free(
				    pte->sublevel + subtable_slot_idx, len))
				return -EBUSY;
		}

		remain -= len;
		subtable_slot_idx = 0;
		pte++;
		slot++;
	}

	return 0;
}

/**
 * Allocate a second level page table.
 * @pg_tbl: Gasket page table pointer.
 * @pte: Extended page table entry under/for which to allocate a second level.
 * @slot: [Device] slot corresponding to pte.
 *
 * Description: Allocate the memory for a second level page table (subtable) at
 *	        the given level 0 entry.  Then call dma_map_page() to map the
 *		second level page table for DMA.  Finally, write the
 *		mapped DMA address into the device page table.
 *
 *		The page table mutex must be held when
 *		gasket_alloc_extended_subtable() is called.
 *
 *		Returns 0 if successful, or a non-zero error otherwise.
 */
static int gasket_alloc_extended_subtable(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *pte,
	u64 __iomem *slot)
{
	ulong page_addr, subtable_bytes;
	dma_addr_t dma_addr;

	/* XXX FIX ME XXX this is inefficient for non-4K page sizes */

	/* GFP_DMA flag must be passed to architectures for which
	 * part of the memory range is not considered DMA'able.
	 * This seems to be the case for Juno board with 4.5.0 Linaro kernel
	 */
	page_addr = get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!page_addr)
		return -ENOMEM;
	pte->page = virt_to_page((void *)page_addr);
	pte->offset = 0;

	subtable_bytes = sizeof(struct gasket_page_table_entry) *
		GASKET_PAGES_PER_SUBTABLE;
	pte->sublevel = vmalloc(subtable_bytes);
	if (!pte->sublevel) {
		free_page(page_addr);
		memset(pte, 0, sizeof(struct gasket_page_table_entry));
		return -ENOMEM;
	}
	memset(pte->sublevel, 0, subtable_bytes);

	/* Map the page into DMA space. */
	if (pg_tbl->dma_ops) {
		pte->dma_addr = dma_map_page(pg_tbl->device, pte->page, 0,
			PAGE_SIZE, DMA_BIDIRECTIONAL);
	} else {
		pte->dma_addr = _no_op_dma_map_page(pg_tbl->device, pte->page,
			0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	}
	/* Wait until the page is mapped. */
	mb();

	/* make the addresses available to the device */
	dma_addr = (pte->dma_addr + pte->offset) | GASKET_VALID_SLOT_FLAG;
	writeq(dma_addr, slot);

	pte->status = PTE_INUSE;

	return 0;
}

/* Unmapping functions */
/*
 * Non-locking entry to unmapping routines.
 * @pg_tbl: Gasket page table structure.
 * @dev_addr: Starting device address of the pages to unmap.
 * @num_pages: The number of device pages to unmap.
 *
 * Description: Version of gasket_unmap_pages that assumes the page table lock
 *              is held.
 */
static void gasket_page_table_unmap_nolock(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	if (!num_pages)
		return;

	if (gasket_addr_is_simple(pg_tbl, dev_addr))
		gasket_unmap_simple_pages(pg_tbl, dev_addr, num_pages);
	else
		gasket_unmap_extended_pages(pg_tbl, dev_addr, num_pages);
}

/*
 * Unmap and release pages mapped to simple addresses.
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: Starting device address of the buffers.
 * @num_pages: The number of device pages to unmap.
 *
 * Description: gasket_simple_unmap_pages calls gasket_perform_unmapping() to
 * unmap and release the buffers in the level 0 page table.
 *
 * The sd_mutex must be held when gasket_unmap_simple_pages() is called.
 */
static void gasket_unmap_simple_pages(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	uint slot = gasket_simple_page_idx(pg_tbl, dev_addr);

	gasket_perform_unmapping(pg_tbl, pg_tbl->entries + slot,
				 pg_tbl->base_slot + slot, num_pages, 1);
}

/**
 * Unmap and release buffers to extended addresses.
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: Starting device address of the pages to unmap.
 * @addr: Starting device address of the buffers.
 * @num_pages: The number of device pages to unmap.
 *
 * Description: gasket_extended_unmap_pages loops over the level 0 page table
 *		entries, and for each calls gasket_perform_unmapping() to unmap
 *		the buffers from the level 1 page [sub]table for that level 0
 *		entry.
 *
 *		The page table mutex must be held when
 *		gasket_unmap_extended_pages() is called.
 */
static void gasket_unmap_extended_pages(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	uint slot_idx, remain, len;
	struct gasket_page_table_entry *pte;
	u64 __iomem *slot_base;

	remain = num_pages;
	slot_idx = gasket_extended_lvl1_page_idx(pg_tbl, dev_addr);
	pte = pg_tbl->entries + pg_tbl->num_simple_entries +
	      gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);

	while (remain > 0) {
		/* TODO: Add check to ensure pte remains valid? */
		len = min(remain, GASKET_PAGES_PER_SUBTABLE - slot_idx);

		if (pte->status == PTE_INUSE) {
			slot_base = (u64 __iomem *)(page_address(pte->page) +
						    pte->offset);
			gasket_perform_unmapping(
				pg_tbl, pte->sublevel + slot_idx,
				slot_base + slot_idx, len, 0);
		}

		remain -= len;
		slot_idx = 0;
		pte++;
	}
}

/*
 * Unmap and release mapped pages.
 * @pg_tbl: Gasket page table pointer.
 * @ptes: Array of page table entries to describe the mapped range, one per
 *        page to unmap.
 * @slots: Device slots corresponding to the mappings described by "ptes".
 *         As with ptes, one element per page to unmap.
 *         If these are simple mappings, these will be address translation
 *         registers. If these are extended mappings, these will be witin a
 *         second-level page table allocated on the host, and so must have
 *	   their __iomem attribute casted away.
 * @num_pages: Number of pages to unmap.
 * @is_simple_mapping: 1 if this is a simple mapping, 0 otherwise.
 *
 * Description: gasket_perform_unmapping() loops through the metadata entries
 *		in a last level page table (simple table or extended subtable),
 *		and for each page:
 *		 - Unmaps the page from DMA space (dma_unmap_page),
 *		 - Returns the page to the OS (gasket_release_page),
 *		The entry in the page table is written to 0. The metadata
 *		type is set to PTE_FREE and the metadata is all reset
 *		to 0.
 *
 *		The page table mutex must be held when this function is called.
 */
static void gasket_perform_unmapping(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *ptes,
	u64 __iomem *slots, uint num_pages, int is_simple_mapping)
{
	int i;
	/*
	 * For each page table entry and corresponding entry in the device's
	 * address translation table:
	 */
	for (i = 0; i < num_pages; i++) {
		/* release the address from the device, */
		if (is_simple_mapping || ptes[i].status == PTE_INUSE)
			writeq(0, &slots[i]);
		else
			((u64 __force *)slots)[i] = 0;
		/* Force sync around the address release. */
		mb();

		/* release the address from the driver, */
		if (ptes[i].status == PTE_INUSE) {
			if (ptes[i].dma_addr) {
				dma_unmap_page(pg_tbl->device, ptes[i].dma_addr,
					       PAGE_SIZE, DMA_FROM_DEVICE);
			}
			if (gasket_release_page(ptes[i].page))
				--pg_tbl->num_active_pages;
		}
		ptes[i].status = PTE_FREE;

		/* and clear the PTE. */
		memset(&ptes[i], 0, sizeof(struct gasket_page_table_entry));
	}
}

/*
 * Free a second level page [sub]table.
 * @pg_tbl: Gasket page table pointer.
 * @pte: Page table entry _pointing_to_ the subtable to free.
 * @slot: Device slot holding a pointer to the sublevel's contents.
 *
 * Description: Safely deallocates a second-level [sub]table by:
 *  - Marking the containing first-level PTE as free
 *  - Setting the corresponding [extended] device slot as NULL
 *  - Unmapping the PTE from DMA space.
 *  - Freeing the subtable's memory.
 *  - Deallocating the page and clearing out the PTE.
 *
 * The page table mutex must be held before this call.
 */
static void gasket_free_extended_subtable(
	struct gasket_page_table *pg_tbl, struct gasket_page_table_entry *pte,
	u64 __iomem *slot)
{
	/* Release the page table from the driver */
	pte->status = PTE_FREE;

	/* Release the page table from the device */
	writeq(0, slot);
	/* Force sync around the address release. */
	mb();

	if (pte->dma_addr)
		dma_unmap_page(pg_tbl->device, pte->dma_addr, PAGE_SIZE,
			       DMA_BIDIRECTIONAL);

	vfree(pte->sublevel);

	if (pte->page)
		free_page((ulong)page_address(pte->page));

	memset(pte, 0, sizeof(struct gasket_page_table_entry));
}

/*
 * Safely return a page to the OS.
 * @page: The page to return to the OS.
 * Returns true if the page was released, false if it was
 * ignored.
 */
static bool gasket_release_page(struct page *page)
{
	if (!page)
		return false;

	if (!PageReserved(page))
		SetPageDirty(page);
	put_page(page);

	return true;
}

/* Evaluates to nonzero if the specified virtual address is simple. */
static inline bool gasket_addr_is_simple(
	struct gasket_page_table *pg_tbl, ulong addr)
{
	return !((addr) & (pg_tbl)->extended_flag);
}

/*
 * Validity checking for simple addresses.
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: The device address to which the pages will be mapped.
 * @num_pages: The number of pages in the range to consider.
 *
 * Description: This call verifies that address translation commutes (from
 * address to/from page + offset) and that the requested page range starts and
 * ends within the set of currently-partitioned simple pages.
 */
static bool gasket_is_simple_dev_addr_bad(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	ulong page_offset = dev_addr & (PAGE_SIZE - 1);
	ulong page_index =
		(dev_addr / PAGE_SIZE) & (pg_tbl->config.total_entries - 1);

	if (gasket_components_to_dev_address(
		pg_tbl, 1, page_index, page_offset) != dev_addr) {
		gasket_pg_tbl_error(
			pg_tbl, "address is invalid, 0x%lX", dev_addr);
		return true;
	}

	if (page_index >= pg_tbl->num_simple_entries) {
		gasket_pg_tbl_error(
			pg_tbl,
			"starting slot at %lu is too large, max is < %u",
			page_index, pg_tbl->num_simple_entries);
		return true;
	}

	if (page_index + num_pages > pg_tbl->num_simple_entries) {
		gasket_pg_tbl_error(
			pg_tbl,
			"ending slot at %lu is too large, max is <= %u",
			page_index + num_pages, pg_tbl->num_simple_entries);
		return true;
	}

	return false;
}

/*
 * Verifies that address translation commutes (from address to/from page +
 * offset) and that the requested page range starts and ends within the set of
 * currently-partitioned simple pages.
 *
 * @pg_tbl: Gasket page table pointer.
 * @dev_addr: The device address to which the pages will be mapped.
 * @num_pages: The number of second-level/sub pages in the range to consider.
 */
static bool gasket_is_extended_dev_addr_bad(
	struct gasket_page_table *pg_tbl, ulong dev_addr, uint num_pages)
{
	/* Starting byte index of dev_addr into the first mapped page */
	ulong page_offset = dev_addr & (PAGE_SIZE - 1);
	ulong page_global_idx, page_lvl0_idx;
	ulong num_lvl0_pages;
	ulong addr;

	/* check if the device address is out of bound */
	addr = dev_addr & ~((pg_tbl)->extended_flag);
	if (addr >> (GASKET_EXTENDED_LVL0_WIDTH + GASKET_EXTENDED_LVL0_SHIFT)) {
		gasket_pg_tbl_error(pg_tbl, "device address out of bound, 0x%p",
				    (void *)dev_addr);
		return true;
	}

	/* Find the starting sub-page index in the space of all sub-pages. */
	page_global_idx = (dev_addr / PAGE_SIZE) &
		(pg_tbl->config.total_entries * GASKET_PAGES_PER_SUBTABLE - 1);

	/* Find the starting level 0 index. */
	page_lvl0_idx = gasket_extended_lvl0_page_idx(pg_tbl, dev_addr);

	/* Get the count of affected level 0 pages. */
	num_lvl0_pages = (num_pages + GASKET_PAGES_PER_SUBTABLE - 1) /
		GASKET_PAGES_PER_SUBTABLE;

	if (gasket_components_to_dev_address(
		pg_tbl, 0, page_global_idx, page_offset) != dev_addr) {
		gasket_pg_tbl_error(
			pg_tbl, "address is invalid, 0x%p", (void *)dev_addr);
		return true;
	}

	if (page_lvl0_idx >= pg_tbl->num_extended_entries) {
		gasket_pg_tbl_error(
			pg_tbl,
			"starting level 0 slot at %lu is too large, max is < "
			"%u", page_lvl0_idx, pg_tbl->num_extended_entries);
		return true;
	}

	if (page_lvl0_idx + num_lvl0_pages > pg_tbl->num_extended_entries) {
		gasket_pg_tbl_error(
			pg_tbl,
			"ending level 0 slot at %lu is too large, max is <= %u",
			page_lvl0_idx + num_lvl0_pages,
			pg_tbl->num_extended_entries);
		return true;
	}

	return false;
}

/*
 * Checks if a range of PTEs is free.
 * @ptes: The set of PTEs to check.
 * @num_entries: The number of PTEs to check.
 *
 * Description: Iterates over the input PTEs to determine if all have been
 * marked as FREE or if any are INUSE. In the former case, 1/true is returned.
 * Otherwise, 0/false is returned.
 *
 * The page table mutex must be held before this call.
 */
static bool gasket_is_pte_range_free(
	struct gasket_page_table_entry *ptes, uint num_entries)
{
	int i;

	for (i = 0; i < num_entries; i++) {
		if (ptes[i].status != PTE_FREE)
			return false;
	}

	return true;
}

/*
 * Actually perform collection.
 * @pg_tbl: Gasket page table structure.
 *
 * Description: Version of gasket_page_table_garbage_collect that assumes the
 *		page table lock is held.
 */
static void gasket_page_table_garbage_collect_nolock(
	struct gasket_page_table *pg_tbl)
{
	struct gasket_page_table_entry *pte;
	u64 __iomem *slot;

	/* XXX FIX ME XXX -- more efficient to keep a usage count */
	/* rather than scanning the second level page tables */

	for (pte = pg_tbl->entries + pg_tbl->num_simple_entries,
	     slot = pg_tbl->base_slot + pg_tbl->num_simple_entries;
	     pte < pg_tbl->entries + pg_tbl->config.total_entries;
	     pte++, slot++) {
		if (pte->status == PTE_INUSE) {
			if (gasket_is_pte_range_free(
				    pte->sublevel, GASKET_PAGES_PER_SUBTABLE))
				gasket_free_extended_subtable(
					pg_tbl, pte, slot);
		}
	}
}

/*
 * Converts components to a device address.
 * @pg_tbl: Gasket page table structure.
 * @is_simple: nonzero if this should be a simple entry, zero otherwise.
 * @page_index: The page index into the respective table.
 * @offset: The offset within the requested page.
 *
 * Simple utility function to convert (simple, page, offset) into a device
 * address.
 * Examples:
 * Simple page 0, offset 32:
 *  Input (0, 0, 32), Output 0x20
 * Simple page 1000, offset 511:
 *  Input (0, 1000, 512), Output 0x3E81FF
 * Extended page 0, offset 32:
 *  Input (0, 0, 32), Output 0x8000000020
 * Extended page 1000, offset 511:
 *  Input (1, 1000, 512), Output 0x8003E81FF
 */
static ulong gasket_components_to_dev_address(
	struct gasket_page_table *pg_tbl, int is_simple, uint page_index,
	uint offset)
{
	ulong lvl0_index, lvl1_index;

	if (is_simple) {
		/* Return simple addresses directly. */
		lvl0_index = page_index & (pg_tbl->config.total_entries - 1);
		return (lvl0_index << GASKET_SIMPLE_PAGE_SHIFT) | offset;
	}

	/*
	 * This could be compressed into fewer statements, but
	 * A) the compiler should optimize it
	 * B) this is not slow
	 * C) this is an uncommon operation
	 * D) this is actually readable this way.
	 */
	lvl0_index = page_index / GASKET_PAGES_PER_SUBTABLE;
	lvl1_index = page_index & (GASKET_PAGES_PER_SUBTABLE - 1);
	return (pg_tbl)->extended_flag |
	       (lvl0_index << GASKET_EXTENDED_LVL0_SHIFT) |
	       (lvl1_index << GASKET_EXTENDED_LVL1_SHIFT) | offset;
}

/*
 * Gets the index of the address' page in the simple table.
 * @pg_tbl: Gasket page table structure.
 * @dev_addr: The address whose page index to retrieve.
 *
 * Description: Treats the input address as a simple address and determines the
 * index of its underlying page in the simple page table (i.e., device address
 * translation registers.
 *
 * Does not perform validity checking.
 */
static int gasket_simple_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr)
{
	return (dev_addr >> GASKET_SIMPLE_PAGE_SHIFT) &
		(pg_tbl->config.total_entries - 1);
}

/*
 * Gets the level 0 page index for the given address.
 * @pg_tbl: Gasket page table structure.
 * @dev_addr: The address whose page index to retrieve.
 *
 * Description: Treats the input address as an extended address and determines
 * the index of its underlying page in the first-level extended page table
 * (i.e., device extended address translation registers).
 *
 * Does not perform validity checking.
 */
static ulong gasket_extended_lvl0_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr)
{
	return (dev_addr >> GASKET_EXTENDED_LVL0_SHIFT) &
	       ((1 << GASKET_EXTENDED_LVL0_WIDTH) - 1);
}

/*
 * Gets the level 1 page index for the given address.
 * @pg_tbl: Gasket page table structure.
 * @dev_addr: The address whose page index to retrieve.
 *
 * Description: Treats the input address as an extended address and determines
 * the index of its underlying page in the second-level extended page table
 * (i.e., host memory pointed to by a first-level page table entry).
 *
 * Does not perform validity checking.
 */
static ulong gasket_extended_lvl1_page_idx(
	struct gasket_page_table *pg_tbl, ulong dev_addr)
{
	return (dev_addr >> GASKET_EXTENDED_LVL1_SHIFT) &
	       (GASKET_PAGES_PER_SUBTABLE - 1);
}

/*
 * Determines whether a host buffer was mapped as coherent memory.
 * @pg_tbl: gasket_page_table structure tracking the host buffer mapping
 * @host_addr: user virtual address within a host buffer
 *
 * Description: A Gasket page_table currently support one contiguous
 * dma range, mapped to one contiguous virtual memory range. Check if the
 * host_addr is within start of page 0, and end of last page, for that range.
 */
static int is_coherent(struct gasket_page_table *pg_tbl, ulong host_addr)
{
	u64 min, max;

	/* whether the host address is within user virt range */
	if (!pg_tbl->coherent_pages)
		return 0;

	min = (u64)pg_tbl->coherent_pages[0].user_virt;
	max = min + PAGE_SIZE * pg_tbl->num_coherent_pages;

	return min <= host_addr && host_addr < max;
}

/*
 * Records the host_addr to coherent dma memory mapping.
 * @gasket_dev: Gasket Device.
 * @size: Size of the virtual address range to map.
 * @dma_address: Dma address within the coherent memory range.
 * @vma: Virtual address we wish to map to coherent memory.
 *
 * Description: For each page in the virtual address range, record the
 * coherent page mgasket_pretapping.
 */
int gasket_set_user_virt(
	struct gasket_dev *gasket_dev, u64 size, dma_addr_t dma_address,
	ulong vma)
{
	int j;
	struct gasket_page_table *pg_tbl;

	unsigned int num_pages = size / PAGE_SIZE;

	/*
	 * TODO: for future chipset, better handling of the case where multiple
	 * page tables are supported on a given device
	 */
	pg_tbl = gasket_dev->page_table[0];
	if (!pg_tbl) {
		gasket_nodev_debug(
			"%s: invalid page table index", __func__);
		return 0;
	}
	for (j = 0; j < num_pages; j++) {
		pg_tbl->coherent_pages[j].user_virt =
			(u64)vma + j * PAGE_SIZE;
	}
	return 0;
}

/*
 * Allocate a block of coherent memory.
 * @gasket_dev: Gasket Device.
 * @size: Size of the memory block.
 * @dma_address: Dma address allocated by the kernel.
 * @index: Index of the gasket_page_table within this Gasket device
 *
 * Description: Allocate a contiguous coherent memory block, DMA'ble
 * by this device.
 */
int gasket_alloc_coherent_memory(struct gasket_dev *gasket_dev, u64 size,
				 dma_addr_t *dma_address, u64 index)
{
	dma_addr_t handle;
	void *mem;
	int j;
	unsigned int num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	const struct gasket_driver_desc *driver_desc =
		gasket_get_driver_desc(gasket_dev);

	if (!gasket_dev->page_table[index])
		return -EFAULT;

	if (num_pages == 0)
		return -EINVAL;

	mem = dma_alloc_coherent(gasket_get_device(gasket_dev),
				 num_pages * PAGE_SIZE, &handle, 0);
	if (!mem)
		goto nomem;

	gasket_dev->page_table[index]->num_coherent_pages = num_pages;

	/* allocate the physical memory block */
	gasket_dev->page_table[index]->coherent_pages =
		kcalloc(num_pages, sizeof(struct gasket_coherent_page_entry),
			GFP_KERNEL);
	if (!gasket_dev->page_table[index]->coherent_pages)
		goto nomem;
	*dma_address = 0;

	gasket_dev->coherent_buffer.length_bytes =
		PAGE_SIZE * (num_pages);
	gasket_dev->coherent_buffer.phys_base = handle;
	gasket_dev->coherent_buffer.virt_base = mem;

	*dma_address = driver_desc->coherent_buffer_description.base;
		for (j = 0; j < num_pages; j++) {
		gasket_dev->page_table[index]->coherent_pages[j].paddr =
			handle + j * PAGE_SIZE;
		gasket_dev->page_table[index]->coherent_pages[j].kernel_virt =
			(u64)mem + j * PAGE_SIZE;
	}

	if (*dma_address == 0)
		goto nomem;
	return 0;

nomem:
	if (mem) {
		dma_free_coherent(gasket_get_device(gasket_dev),
				  num_pages * PAGE_SIZE, mem, handle);
	}

	if (gasket_dev->page_table[index]->coherent_pages) {
		kfree(gasket_dev->page_table[index]->coherent_pages);
		gasket_dev->page_table[index]->coherent_pages = 0;
	}
	gasket_dev->page_table[index]->num_coherent_pages = 0;
	return -ENOMEM;
}

/*
 * Free a block of coherent memory.
 * @gasket_dev: Gasket Device.
 * @size: Size of the memory block.
 * @dma_address: Dma address allocated by the kernel.
 * @index: Index of the gasket_page_table within this Gasket device
 *
 * Description: Release memory allocated thru gasket_alloc_coherent_memory.
 */
int gasket_free_coherent_memory(struct gasket_dev *gasket_dev, u64 size,
				dma_addr_t dma_address, u64 index)
{
	const struct gasket_driver_desc *driver_desc;

	if (!gasket_dev->page_table[index])
		return -EFAULT;

	driver_desc = gasket_get_driver_desc(gasket_dev);

	if (driver_desc->coherent_buffer_description.base != dma_address)
		return -EADDRNOTAVAIL;

	if (gasket_dev->coherent_buffer.length_bytes) {
		dma_free_coherent(gasket_get_device(gasket_dev),
				  gasket_dev->coherent_buffer.length_bytes,
				  gasket_dev->coherent_buffer.virt_base,
				  gasket_dev->coherent_buffer.phys_base);
		gasket_dev->coherent_buffer.length_bytes = 0;
		gasket_dev->coherent_buffer.virt_base = NULL;
		gasket_dev->coherent_buffer.phys_base = 0;
	}
	return 0;
}

/*
 * Release all coherent memory.
 * @gasket_dev: Gasket Device.
 * @index: Index of the gasket_page_table within this Gasket device
 *
 * Description: Release all memory allocated thru gasket_alloc_coherent_memory.
 */
void gasket_free_coherent_memory_all(
	struct gasket_dev *gasket_dev, u64 index)
{
	if (!gasket_dev->page_table[index])
		return;

	if (gasket_dev->coherent_buffer.length_bytes) {
		dma_free_coherent(gasket_get_device(gasket_dev),
				  gasket_dev->coherent_buffer.length_bytes,
				  gasket_dev->coherent_buffer.virt_base,
				  gasket_dev->coherent_buffer.phys_base);
		gasket_dev->coherent_buffer.length_bytes = 0;
		gasket_dev->coherent_buffer.virt_base = NULL;
		gasket_dev->coherent_buffer.phys_base = 0;
	}
}
