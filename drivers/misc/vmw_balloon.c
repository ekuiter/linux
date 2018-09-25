// SPDX-License-Identifier: GPL-2.0
/*
 * VMware Balloon driver.
 *
 * Copyright (C) 2000-2018, VMware, Inc. All Rights Reserved.
 *
 * This is VMware physical memory management driver for Linux. The driver
 * acts like a "balloon" that can be inflated to reclaim physical pages by
 * reserving them in the guest and invalidating them in the monitor,
 * freeing up the underlying machine pages so they can be allocated to
 * other guests.  The balloon can also be deflated to allow the guest to
 * use more physical memory. Higher level policies can control the sizes
 * of balloons in VMs in order to manage physical memory resources.
 */

//#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/vmw_vmci_defs.h>
#include <linux/vmw_vmci_api.h>
#include <asm/hypervisor.h>

MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware Memory Control (Balloon) Driver");
MODULE_VERSION("1.5.0.0-k");
MODULE_ALIAS("dmi:*:svnVMware*:*");
MODULE_ALIAS("vmware_vmmemctl");
MODULE_LICENSE("GPL");

/*
 * Use __GFP_HIGHMEM to allow pages from HIGHMEM zone. We don't allow wait
 * (__GFP_RECLAIM) for huge page allocations. Use __GFP_NOWARN, to suppress page
 * allocation failure warnings. Disallow access to emergency low-memory pools.
 */
#define VMW_HUGE_PAGE_ALLOC_FLAGS	(__GFP_HIGHMEM|__GFP_NOWARN|	\
					 __GFP_NOMEMALLOC)

/*
 * Use __GFP_HIGHMEM to allow pages from HIGHMEM zone. We allow lightweight
 * reclamation (__GFP_NORETRY). Use __GFP_NOWARN, to suppress page allocation
 * failure warnings. Disallow access to emergency low-memory pools.
 */
#define VMW_PAGE_ALLOC_FLAGS		(__GFP_HIGHMEM|__GFP_NOWARN|	\
					 __GFP_NOMEMALLOC|__GFP_NORETRY)

/* Maximum number of refused pages we accumulate during inflation cycle */
#define VMW_BALLOON_MAX_REFUSED		16

/*
 * Hypervisor communication port definitions.
 */
#define VMW_BALLOON_HV_PORT		0x5670
#define VMW_BALLOON_HV_MAGIC		0x456c6d6f
#define VMW_BALLOON_GUEST_ID		1	/* Linux */

enum vmwballoon_capabilities {
	/*
	 * Bit 0 is reserved and not associated to any capability.
	 */
	VMW_BALLOON_BASIC_CMDS			= (1 << 1),
	VMW_BALLOON_BATCHED_CMDS		= (1 << 2),
	VMW_BALLOON_BATCHED_2M_CMDS		= (1 << 3),
	VMW_BALLOON_SIGNALLED_WAKEUP_CMD	= (1 << 4),
};

#define VMW_BALLOON_CAPABILITIES	(VMW_BALLOON_BASIC_CMDS \
					| VMW_BALLOON_BATCHED_CMDS \
					| VMW_BALLOON_BATCHED_2M_CMDS \
					| VMW_BALLOON_SIGNALLED_WAKEUP_CMD)

#define VMW_BALLOON_2M_ORDER		(PMD_SHIFT - PAGE_SHIFT)

enum vmballoon_page_size_type {
	VMW_BALLOON_4K_PAGE,
	VMW_BALLOON_2M_PAGE,
	VMW_BALLOON_LAST_SIZE = VMW_BALLOON_2M_PAGE
};

#define VMW_BALLOON_NUM_PAGE_SIZES	(VMW_BALLOON_LAST_SIZE + 1)

enum vmballoon_op_stat_type {
	VMW_BALLOON_OP_STAT,
	VMW_BALLOON_OP_FAIL_STAT
};

#define VMW_BALLOON_OP_STAT_TYPES	(VMW_BALLOON_OP_FAIL_STAT + 1)

/**
 * enum vmballoon_cmd_type - backdoor commands.
 *
 * Availability of the commands is as followed:
 *
 * %VMW_BALLOON_CMD_START, %VMW_BALLOON_CMD_GET_TARGET and
 * %VMW_BALLOON_CMD_GUEST_ID are always available.
 *
 * If the host reports %VMW_BALLOON_BASIC_CMDS are supported then
 * %VMW_BALLOON_CMD_LOCK and %VMW_BALLOON_CMD_UNLOCK commands are available.
 *
 * If the host reports %VMW_BALLOON_BATCHED_CMDS are supported then
 * %VMW_BALLOON_CMD_BATCHED_LOCK and VMW_BALLOON_CMD_BATCHED_UNLOCK commands
 * are available.
 *
 * If the host reports %VMW_BALLOON_BATCHED_2M_CMDS are supported then
 * %VMW_BALLOON_CMD_BATCHED_2M_LOCK and %VMW_BALLOON_CMD_BATCHED_2M_UNLOCK
 * are supported.
 *
 * If the host reports  VMW_BALLOON_SIGNALLED_WAKEUP_CMD is supported then
 * VMW_BALLOON_CMD_VMCI_DOORBELL_SET command is supported.
 *
 * @VMW_BALLOON_CMD_START: Communicating supported version with the hypervisor.
 * @VMW_BALLOON_CMD_GET_TARGET: Gets the balloon target size.
 * @VMW_BALLOON_CMD_LOCK: Informs the hypervisor about a ballooned page.
 * @VMW_BALLOON_CMD_UNLOCK: Informs the hypervisor about a page that is about
 *			    to be deflated from the balloon.
 * @VMW_BALLOON_CMD_GUEST_ID: Informs the hypervisor about the type of OS that
 *			      runs in the VM.
 * @VMW_BALLOON_CMD_BATCHED_LOCK: Inform the hypervisor about a batch of
 *				  ballooned pages (up to 512).
 * @VMW_BALLOON_CMD_BATCHED_UNLOCK: Inform the hypervisor about a batch of
 *				  pages that are about to be deflated from the
 *				  balloon (up to 512).
 * @VMW_BALLOON_CMD_BATCHED_2M_LOCK: Similar to @VMW_BALLOON_CMD_BATCHED_LOCK
 *				     for 2MB pages.
 * @VMW_BALLOON_CMD_BATCHED_2M_UNLOCK: Similar to
 *				       @VMW_BALLOON_CMD_BATCHED_UNLOCK for 2MB
 *				       pages.
 * @VMW_BALLOON_CMD_VMCI_DOORBELL_SET: A command to set doorbell notification
 *				       that would be invoked when the balloon
 *				       size changes.
 * @VMW_BALLOON_CMD_LAST: Value of the last command.
 */
enum vmballoon_cmd_type {
	VMW_BALLOON_CMD_START,
	VMW_BALLOON_CMD_GET_TARGET,
	VMW_BALLOON_CMD_LOCK,
	VMW_BALLOON_CMD_UNLOCK,
	VMW_BALLOON_CMD_GUEST_ID,
	/* No command 5 */
	VMW_BALLOON_CMD_BATCHED_LOCK = 6,
	VMW_BALLOON_CMD_BATCHED_UNLOCK,
	VMW_BALLOON_CMD_BATCHED_2M_LOCK,
	VMW_BALLOON_CMD_BATCHED_2M_UNLOCK,
	VMW_BALLOON_CMD_VMCI_DOORBELL_SET,
	VMW_BALLOON_CMD_LAST = VMW_BALLOON_CMD_VMCI_DOORBELL_SET,
};

#define VMW_BALLOON_CMD_NUM	(VMW_BALLOON_CMD_LAST + 1)

enum vmballoon_error_codes {
	VMW_BALLOON_SUCCESS,
	VMW_BALLOON_ERROR_CMD_INVALID,
	VMW_BALLOON_ERROR_PPN_INVALID,
	VMW_BALLOON_ERROR_PPN_LOCKED,
	VMW_BALLOON_ERROR_PPN_UNLOCKED,
	VMW_BALLOON_ERROR_PPN_PINNED,
	VMW_BALLOON_ERROR_PPN_NOTNEEDED,
	VMW_BALLOON_ERROR_RESET,
	VMW_BALLOON_ERROR_BUSY
};

#define VMW_BALLOON_SUCCESS_WITH_CAPABILITIES	(0x03000000)

#define VMW_BALLOON_CMD_WITH_TARGET_MASK			\
	((1UL << VMW_BALLOON_CMD_GET_TARGET)		|	\
	 (1UL << VMW_BALLOON_CMD_LOCK)			|	\
	 (1UL << VMW_BALLOON_CMD_UNLOCK)		|	\
	 (1UL << VMW_BALLOON_CMD_BATCHED_LOCK)		|	\
	 (1UL << VMW_BALLOON_CMD_BATCHED_UNLOCK)	|	\
	 (1UL << VMW_BALLOON_CMD_BATCHED_2M_LOCK)	|	\
	 (1UL << VMW_BALLOON_CMD_BATCHED_2M_UNLOCK))

static const char * const vmballoon_cmd_names[] = {
	[VMW_BALLOON_CMD_START]			= "start",
	[VMW_BALLOON_CMD_GET_TARGET]		= "target",
	[VMW_BALLOON_CMD_LOCK]			= "lock",
	[VMW_BALLOON_CMD_UNLOCK]		= "unlock",
	[VMW_BALLOON_CMD_GUEST_ID]		= "guestType",
	[VMW_BALLOON_CMD_BATCHED_LOCK]		= "batchLock",
	[VMW_BALLOON_CMD_BATCHED_UNLOCK]	= "batchUnlock",
	[VMW_BALLOON_CMD_BATCHED_2M_LOCK]	= "2m-lock",
	[VMW_BALLOON_CMD_BATCHED_2M_UNLOCK]	= "2m-unlock",
	[VMW_BALLOON_CMD_VMCI_DOORBELL_SET]	= "doorbellSet"
};

enum vmballoon_stat_page {
	VMW_BALLOON_PAGE_STAT_ALLOC,
	VMW_BALLOON_PAGE_STAT_ALLOC_FAIL,
	VMW_BALLOON_PAGE_STAT_REFUSED_ALLOC,
	VMW_BALLOON_PAGE_STAT_REFUSED_FREE,
	VMW_BALLOON_PAGE_STAT_FREE,
	VMW_BALLOON_PAGE_STAT_LAST = VMW_BALLOON_PAGE_STAT_FREE
};

#define VMW_BALLOON_PAGE_STAT_NUM	(VMW_BALLOON_PAGE_STAT_LAST + 1)

enum vmballoon_stat_general {
	VMW_BALLOON_STAT_TIMER,
	VMW_BALLOON_STAT_DOORBELL,
	VMW_BALLOON_STAT_LAST = VMW_BALLOON_STAT_DOORBELL
};

#define VMW_BALLOON_STAT_NUM		(VMW_BALLOON_STAT_LAST + 1)


static DEFINE_STATIC_KEY_TRUE(vmw_balloon_batching);
static DEFINE_STATIC_KEY_FALSE(balloon_stat_enabled);

struct vmballoon_page_size {
	/* list of reserved physical pages */
	struct list_head pages;

	/* transient list of non-balloonable pages */
	struct list_head refused_pages;
	unsigned int n_refused_pages;
};

/**
 * struct vmballoon_batch_entry - a batch entry for lock or unlock.
 *
 * @status: the status of the operation, which is written by the hypervisor.
 * @reserved: reserved for future use. Must be set to zero.
 * @pfn: the physical frame number of the page to be locked or unlocked.
 */
struct vmballoon_batch_entry {
	u64 status : 5;
	u64 reserved : PAGE_SHIFT - 5;
	u64 pfn : 52;
} __packed;

struct vmballoon {
	struct vmballoon_page_size page_sizes[VMW_BALLOON_NUM_PAGE_SIZES];

	/* supported page sizes. 1 == 4k pages only, 2 == 4k and 2m pages */
	unsigned supported_page_sizes;

	/* balloon size in pages */
	unsigned int size;
	unsigned int target;

	/* reset flag */
	bool reset_required;

	unsigned long capabilities;

	/**
	 * @batch_page: pointer to communication batch page.
	 *
	 * When batching is used, batch_page points to a page, which holds up to
	 * %VMW_BALLOON_BATCH_MAX_PAGES entries for locking or unlocking.
	 */
	struct vmballoon_batch_entry *batch_page;

	unsigned int batch_max_pages;
	struct page *page;

	/* statistics */
	struct vmballoon_stats *stats;

#ifdef CONFIG_DEBUG_FS
	/* debugfs file exporting statistics */
	struct dentry *dbg_entry;
#endif

	struct delayed_work dwork;

	struct vmci_handle vmci_doorbell;

	/**
	 * @conf_sem: semaphore to protect the configuration and the statistics.
	 */
	struct rw_semaphore conf_sem;
};

static struct vmballoon balloon;

struct vmballoon_stats {
	/* timer / doorbell operations */
	atomic64_t general_stat[VMW_BALLOON_STAT_NUM];

	/* allocation statistics for huge and small pages */
	atomic64_t
	       page_stat[VMW_BALLOON_PAGE_STAT_NUM][VMW_BALLOON_NUM_PAGE_SIZES];

	/* Monitor operations: total operations, and failures */
	atomic64_t ops[VMW_BALLOON_CMD_NUM][VMW_BALLOON_OP_STAT_TYPES];
};

static inline bool is_vmballoon_stats_on(void)
{
	return IS_ENABLED(CONFIG_DEBUG_FS) &&
		static_branch_unlikely(&balloon_stat_enabled);
}

static inline void vmballoon_stats_op_inc(struct vmballoon *b, unsigned int op,
					  enum vmballoon_op_stat_type type)
{
	if (is_vmballoon_stats_on())
		atomic64_inc(&b->stats->ops[op][type]);
}

static inline void vmballoon_stats_gen_inc(struct vmballoon *b,
					   enum vmballoon_stat_general stat)
{
	if (is_vmballoon_stats_on())
		atomic64_inc(&b->stats->general_stat[stat]);
}

static inline void vmballoon_stats_gen_add(struct vmballoon *b,
					   enum vmballoon_stat_general stat,
					   unsigned int val)
{
	if (is_vmballoon_stats_on())
		atomic64_add(val, &b->stats->general_stat[stat]);
}

static inline void vmballoon_stats_page_inc(struct vmballoon *b,
					    enum vmballoon_stat_page stat,
					    bool is_2m_page)
{
	if (is_vmballoon_stats_on())
		atomic64_inc(&b->stats->page_stat[stat][is_2m_page]);
}

static inline unsigned long
__vmballoon_cmd(struct vmballoon *b, unsigned long cmd, unsigned long arg1,
		unsigned long arg2, unsigned long *result)
{
	unsigned long status, dummy1, dummy2, dummy3, local_result;

	vmballoon_stats_op_inc(b, cmd, VMW_BALLOON_OP_STAT);

	asm volatile ("inl %%dx" :
		"=a"(status),
		"=c"(dummy1),
		"=d"(dummy2),
		"=b"(local_result),
		"=S"(dummy3) :
		"0"(VMW_BALLOON_HV_MAGIC),
		"1"(cmd),
		"2"(VMW_BALLOON_HV_PORT),
		"3"(arg1),
		"4"(arg2) :
		"memory");

	/* update the result if needed */
	if (result)
		*result = (cmd == VMW_BALLOON_CMD_START) ? dummy1 :
							   local_result;

	/* update target when applicable */
	if (status == VMW_BALLOON_SUCCESS &&
	    ((1ul << cmd) & VMW_BALLOON_CMD_WITH_TARGET_MASK))
		b->target = local_result;

	if (status != VMW_BALLOON_SUCCESS &&
	    status != VMW_BALLOON_SUCCESS_WITH_CAPABILITIES) {
		vmballoon_stats_op_inc(b, cmd, VMW_BALLOON_OP_FAIL_STAT);
		pr_debug("%s: %s [0x%lx,0x%lx) failed, returned %ld\n",
			 __func__, vmballoon_cmd_names[cmd], arg1, arg2,
			 status);
	}

	/* mark reset required accordingly */
	if (status == VMW_BALLOON_ERROR_RESET)
		b->reset_required = true;

	return status;
}

static __always_inline unsigned long
vmballoon_cmd(struct vmballoon *b, unsigned long cmd, unsigned long arg1,
	      unsigned long arg2)
{
	unsigned long dummy;

	return __vmballoon_cmd(b, cmd, arg1, arg2, &dummy);
}

/*
 * Send "start" command to the host, communicating supported version
 * of the protocol.
 */
static bool vmballoon_send_start(struct vmballoon *b, unsigned long req_caps)
{
	unsigned long status, capabilities;
	bool success;

	status = __vmballoon_cmd(b, VMW_BALLOON_CMD_START, req_caps, 0,
				 &capabilities);

	switch (status) {
	case VMW_BALLOON_SUCCESS_WITH_CAPABILITIES:
		b->capabilities = capabilities;
		success = true;
		break;
	case VMW_BALLOON_SUCCESS:
		b->capabilities = VMW_BALLOON_BASIC_CMDS;
		success = true;
		break;
	default:
		success = false;
	}

	/*
	 * 2MB pages are only supported with batching. If batching is for some
	 * reason disabled, do not use 2MB pages, since otherwise the legacy
	 * mechanism is used with 2MB pages, causing a failure.
	 */
	if ((b->capabilities & VMW_BALLOON_BATCHED_2M_CMDS) &&
	    (b->capabilities & VMW_BALLOON_BATCHED_CMDS))
		b->supported_page_sizes = 2;
	else
		b->supported_page_sizes = 1;

	return success;
}

/*
 * Communicate guest type to the host so that it can adjust ballooning
 * algorithm to the one most appropriate for the guest. This command
 * is normally issued after sending "start" command and is part of
 * standard reset sequence.
 */
static bool vmballoon_send_guest_id(struct vmballoon *b)
{
	unsigned long status;

	status = vmballoon_cmd(b, VMW_BALLOON_CMD_GUEST_ID,
			       VMW_BALLOON_GUEST_ID, 0);

	if (status == VMW_BALLOON_SUCCESS)
		return true;

	return false;
}

static u16 vmballoon_page_size(bool is_2m_page)
{
	if (is_2m_page)
		return 1 << VMW_BALLOON_2M_ORDER;

	return 1;
}

/**
 * vmballoon_send_get_target() - Retrieve desired balloon size from the host.
 *
 * @b: pointer to the balloon.
 *
 * Return: zero on success, EINVAL if limit does not fit in 32-bit, as required
 * by the host-guest protocol and EIO if an error occurred in communicating with
 * the host.
 */
static int vmballoon_send_get_target(struct vmballoon *b)
{
	unsigned long status;
	unsigned long limit;

	limit = totalram_pages;

	/* Ensure limit fits in 32-bits */
	if (limit != (u32)limit)
		return -EINVAL;

	status = vmballoon_cmd(b, VMW_BALLOON_CMD_GET_TARGET, limit, 0);

	return status == VMW_BALLOON_SUCCESS ? 0 : -EIO;
}

static struct page *vmballoon_alloc_page(bool is_2m_page)
{
	if (is_2m_page)
		return alloc_pages(VMW_HUGE_PAGE_ALLOC_FLAGS,
				   VMW_BALLOON_2M_ORDER);

	return alloc_page(VMW_PAGE_ALLOC_FLAGS);
}

static void vmballoon_free_page(struct page *page, bool is_2m_page)
{
	if (is_2m_page)
		__free_pages(page, VMW_BALLOON_2M_ORDER);
	else
		__free_page(page);
}

/*
 * Quickly release all pages allocated for the balloon. This function is
 * called when host decides to "reset" balloon for one reason or another.
 * Unlike normal "deflate" we do not (shall not) notify host of the pages
 * being released.
 */
static void vmballoon_pop(struct vmballoon *b)
{
	struct page *page, *next;
	unsigned is_2m_pages;

	for (is_2m_pages = 0; is_2m_pages < VMW_BALLOON_NUM_PAGE_SIZES;
			is_2m_pages++) {
		struct vmballoon_page_size *page_size =
				&b->page_sizes[is_2m_pages];
		u16 size_per_page = vmballoon_page_size(is_2m_pages);

		list_for_each_entry_safe(page, next, &page_size->pages, lru) {
			list_del(&page->lru);
			vmballoon_free_page(page, is_2m_pages);
			vmballoon_stats_page_inc(b, VMW_BALLOON_PAGE_STAT_FREE,
						 is_2m_pages);
			b->size -= size_per_page;
			cond_resched();
		}
	}

	/* Clearing the batch_page unconditionally has no adverse effect */
	free_page((unsigned long)b->batch_page);
	b->batch_page = NULL;
}

/**
 * vmballoon_status_page - returns the status of (un)lock operation
 *
 * @b: pointer to the balloon.
 * @idx: index for the page for which the operation is performed.
 * @p: pointer to where the page struct is returned.
 *
 * Following a lock or unlock operation, returns the status of the operation for
 * an individual page. Provides the page that the operation was performed on on
 * the @page argument.
 *
 * Returns: The status of a lock or unlock operation for an individual page.
 */
static unsigned long vmballoon_status_page(struct vmballoon *b, int idx,
					   struct page **p)
{
	if (static_branch_likely(&vmw_balloon_batching)) {
		/* batching mode */
		*p = pfn_to_page(b->batch_page[idx].pfn);
		return b->batch_page[idx].status;
	}

	/* non-batching mode */
	*p = b->page;

	/*
	 * If a failure occurs, the indication will be provided in the status
	 * of the entire operation, which is considered before the individual
	 * page status. So for non-batching mode, the indication is always of
	 * success.
	 */
	return VMW_BALLOON_SUCCESS;
}

/**
 * vmballoon_lock_op - notifies the host about inflated/deflated pages.
 * @b: pointer to the balloon.
 * @num_pages: number of inflated/deflated pages.
 * @is_2m_pages: whether the page(s) are 2M (or 4k).
 * @lock: whether the operation is lock (or unlock).
 *
 * Notify the host about page(s) that were ballooned (or removed from the
 * balloon) so that host can use it without fear that guest will need it (or
 * stop using them since the VM does). Host may reject some pages, we need to
 * check the return value and maybe submit a different page. The pages that are
 * inflated/deflated are pointed by @b->page.
 *
 * Return: result as provided by the hypervisor.
 */
static unsigned long vmballoon_lock_op(struct vmballoon *b,
				       unsigned int num_pages,
				       bool is_2m_pages, bool lock)
{
	unsigned long cmd, pfn;

	if (static_branch_likely(&vmw_balloon_batching)) {
		if (lock)
			cmd = is_2m_pages ? VMW_BALLOON_CMD_BATCHED_2M_LOCK :
					    VMW_BALLOON_CMD_BATCHED_LOCK;
		else
			cmd = is_2m_pages ? VMW_BALLOON_CMD_BATCHED_2M_UNLOCK :
					    VMW_BALLOON_CMD_BATCHED_UNLOCK;

		pfn = PHYS_PFN(virt_to_phys(b->batch_page));
	} else {
		cmd = lock ? VMW_BALLOON_CMD_LOCK : VMW_BALLOON_CMD_UNLOCK;
		pfn = page_to_pfn(b->page);

		/* In non-batching mode, PFNs must fit in 32-bit */
		if (unlikely(pfn != (u32)pfn))
			return VMW_BALLOON_ERROR_PPN_INVALID;
	}

	return vmballoon_cmd(b, cmd, pfn, num_pages);
}

static int vmballoon_lock(struct vmballoon *b, unsigned int num_pages,
			  bool is_2m_pages)
{
	unsigned long batch_status;
	int i;
	u16 size_per_page = vmballoon_page_size(is_2m_pages);

	batch_status = vmballoon_lock_op(b, num_pages, is_2m_pages, true);

	for (i = 0; i < num_pages; i++) {
		unsigned long status;
		struct page *p;
		struct vmballoon_page_size *page_size =
				&b->page_sizes[is_2m_pages];

		status = vmballoon_status_page(b, i, &p);

		/*
		 * Failure of the whole batch overrides a single operation
		 * results.
		 */
		if (batch_status != VMW_BALLOON_SUCCESS)
			status = batch_status;

		if (status == VMW_BALLOON_SUCCESS) {
			/* track allocated page */
			list_add(&p->lru, &page_size->pages);

			/* update balloon size */
			b->size += size_per_page;
			continue;
		}

		/* Error occurred */
		vmballoon_stats_page_inc(b, VMW_BALLOON_PAGE_STAT_REFUSED_ALLOC,
					 is_2m_pages);

		/*
		 * Place page on the list of non-balloonable pages
		 * and retry allocation, unless we already accumulated
		 * too many of them, in which case take a breather.
		 */
		list_add(&p->lru, &page_size->refused_pages);
		page_size->n_refused_pages++;
	}

	return batch_status == VMW_BALLOON_SUCCESS ? 0 : -EIO;
}

/*
 * Release the page allocated for the balloon. Note that we first notify
 * the host so it can make sure the page will be available for the guest
 * to use, if needed.
 */
static int vmballoon_unlock(struct vmballoon *b, unsigned int num_pages,
			    bool is_2m_pages)
{
	int i;
	unsigned long batch_status;
	u16 size_per_page = vmballoon_page_size(is_2m_pages);

	batch_status = vmballoon_lock_op(b, num_pages, is_2m_pages, false);

	for (i = 0; i < num_pages; i++) {
		struct vmballoon_page_size *page_size;
		unsigned long status;
		struct page *p;

		status = vmballoon_status_page(b, i, &p);
		page_size = &b->page_sizes[is_2m_pages];

		/*
		 * Failure of the whole batch overrides a single operation
		 * results.
		 */
		if (batch_status != VMW_BALLOON_SUCCESS)
			status = batch_status;

		if (status != VMW_BALLOON_SUCCESS) {
			/*
			 * That page wasn't successfully unlocked by the
			 * hypervisor, re-add it to the list of pages owned by
			 * the balloon driver.
			 */
			list_add(&p->lru, &page_size->pages);
		} else {
			/* deallocate page */
			vmballoon_free_page(p, is_2m_pages);
			vmballoon_stats_page_inc(b, VMW_BALLOON_PAGE_STAT_FREE,
						 is_2m_pages);

			/* update balloon size */
			b->size -= size_per_page;
		}
	}

	return batch_status == VMW_BALLOON_SUCCESS ? 0 : -EIO;
}

/*
 * Release pages that were allocated while attempting to inflate the
 * balloon but were refused by the host for one reason or another.
 */
static void vmballoon_release_refused_pages(struct vmballoon *b,
		bool is_2m_pages)
{
	struct page *page, *next;
	struct vmballoon_page_size *page_size =
			&b->page_sizes[is_2m_pages];

	list_for_each_entry_safe(page, next, &page_size->refused_pages, lru) {
		list_del(&page->lru);
		vmballoon_free_page(page, is_2m_pages);
		vmballoon_stats_page_inc(b, VMW_BALLOON_PAGE_STAT_REFUSED_FREE,
					 is_2m_pages);
	}

	page_size->n_refused_pages = 0;
}

static void vmballoon_add_page(struct vmballoon *b, int idx, struct page *p)
{
	if (static_branch_likely(&vmw_balloon_batching))
		b->batch_page[idx] = (struct vmballoon_batch_entry)
					{ .pfn = page_to_pfn(p) };
	else
		b->page = p;
}

/**
 * vmballoon_change - retrieve the required balloon change
 *
 * @b: pointer for the balloon.
 *
 * Return: the required change for the balloon size. A positive number
 * indicates inflation, a negative number indicates a deflation.
 */
static int64_t vmballoon_change(struct vmballoon *b)
{
	int64_t size, target;

	size = b->size;
	target = b->target;

	/*
	 * We must cast first because of int sizes
	 * Otherwise we might get huge positives instead of negatives
	 */

	if (b->reset_required)
		return 0;

	/* consider a 2MB slack on deflate, unless the balloon is emptied */
	if (target < size && size - target < vmballoon_page_size(true) &&
	    target != 0)
		return 0;

	return target - size;
}

/*
 * Inflate the balloon towards its target size. Note that we try to limit
 * the rate of allocation to make sure we are not choking the rest of the
 * system.
 */
static void vmballoon_inflate(struct vmballoon *b)
{
	unsigned int num_pages = 0;
	int error = 0;
	bool is_2m_pages;

	/*
	 * First try NOSLEEP page allocations to inflate balloon.
	 *
	 * If we do not throttle nosleep allocations, we can drain all
	 * free pages in the guest quickly (if the balloon target is high).
	 * As a side-effect, draining free pages helps to inform (force)
	 * the guest to start swapping if balloon target is not met yet,
	 * which is a desired behavior. However, balloon driver can consume
	 * all available CPU cycles if too many pages are allocated in a
	 * second. Therefore, we throttle nosleep allocations even when
	 * the guest is not under memory pressure. OTOH, if we have already
	 * predicted that the guest is under memory pressure, then we
	 * slowdown page allocations considerably.
	 */

	/*
	 * Start with no sleep allocation rate which may be higher
	 * than sleeping allocation rate.
	 */
	is_2m_pages = b->supported_page_sizes == VMW_BALLOON_NUM_PAGE_SIZES;

	while ((int64_t)(num_pages * vmballoon_page_size(is_2m_pages)) <
	       vmballoon_change(b)) {
		struct page *page;

		vmballoon_stats_page_inc(b, VMW_BALLOON_PAGE_STAT_ALLOC,
					 is_2m_pages);

		page = vmballoon_alloc_page(is_2m_pages);
		if (!page) {
			vmballoon_stats_page_inc(b,
				VMW_BALLOON_PAGE_STAT_ALLOC_FAIL, is_2m_pages);

			if (is_2m_pages) {
				vmballoon_lock(b, num_pages, true);

				/*
				 * ignore errors from locking as we now switch
				 * to 4k pages and we might get different
				 * errors.
				 */

				num_pages = 0;
				is_2m_pages = false;
				continue;
			}
			break;
		}

		vmballoon_add_page(b, num_pages++, page);
		if (num_pages == b->batch_max_pages) {
			struct vmballoon_page_size *page_size =
					&b->page_sizes[is_2m_pages];

			error = vmballoon_lock(b, num_pages, is_2m_pages);

			num_pages = 0;

			/*
			 * Stop allocating this page size if we already
			 * accumulated too many pages that the hypervisor
			 * refused.
			 */
			if (page_size->n_refused_pages >=
			    VMW_BALLOON_MAX_REFUSED) {
				if (!is_2m_pages)
					break;

				/*
				 * Release the refused pages as we move to 4k
				 * pages.
				 */
				vmballoon_release_refused_pages(b, true);
				is_2m_pages = true;
			}

			if (error)
				break;
		}

		cond_resched();
	}

	if (num_pages > 0)
		vmballoon_lock(b, num_pages, is_2m_pages);

	vmballoon_release_refused_pages(b, true);
	vmballoon_release_refused_pages(b, false);
}

/*
 * Decrease the size of the balloon allowing guest to use more memory.
 */
static void vmballoon_deflate(struct vmballoon *b)
{
	unsigned is_2m_pages;

	/* free pages to reach target */
	for (is_2m_pages = 0; is_2m_pages < b->supported_page_sizes;
			is_2m_pages++) {
		struct page *page, *next;
		unsigned int num_pages = 0;
		struct vmballoon_page_size *page_size =
				&b->page_sizes[is_2m_pages];

		list_for_each_entry_safe(page, next, &page_size->pages, lru) {
			if ((int64_t)(num_pages *
				      vmballoon_page_size(is_2m_pages)) >=
					-vmballoon_change(b))
				break;

			list_del(&page->lru);
			vmballoon_add_page(b, num_pages++, page);

			if (num_pages == b->batch_max_pages) {
				int error;

				error = vmballoon_unlock(b, num_pages,
						       is_2m_pages);
				num_pages = 0;
				if (error)
					return;
			}

			cond_resched();
		}

		if (num_pages > 0)
			vmballoon_unlock(b, num_pages, is_2m_pages);
	}
}

/**
 * vmballoon_deinit_batching - disables batching mode.
 *
 * @b: pointer to &struct vmballoon.
 *
 * Disables batching, by deallocating the page for communication with the
 * hypervisor and disabling the static key to indicate that batching is off.
 */
static void vmballoon_deinit_batching(struct vmballoon *b)
{
	free_page((unsigned long)b->batch_page);
	b->batch_page = NULL;
	static_branch_disable(&vmw_balloon_batching);
	b->batch_max_pages = 1;
}

/**
 * vmballoon_init_batching - enable batching mode.
 *
 * @b: pointer to &struct vmballoon.
 *
 * Enables batching, by allocating a page for communication with the hypervisor
 * and enabling the static_key to use batching.
 *
 * Return: zero on success or an appropriate error-code.
 */
static int vmballoon_init_batching(struct vmballoon *b)
{
	struct page *page;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	b->batch_page = page_address(page);
	b->batch_max_pages = PAGE_SIZE / sizeof(struct vmballoon_batch_entry);

	static_branch_enable(&vmw_balloon_batching);

	return 0;
}

/*
 * Receive notification and resize balloon
 */
static void vmballoon_doorbell(void *client_data)
{
	struct vmballoon *b = client_data;

	vmballoon_stats_gen_inc(b, VMW_BALLOON_STAT_DOORBELL);

	mod_delayed_work(system_freezable_wq, &b->dwork, 0);
}

/*
 * Clean up vmci doorbell
 */
static void vmballoon_vmci_cleanup(struct vmballoon *b)
{
	vmballoon_cmd(b, VMW_BALLOON_CMD_VMCI_DOORBELL_SET,
		      VMCI_INVALID_ID, VMCI_INVALID_ID);

	if (!vmci_handle_is_invalid(b->vmci_doorbell)) {
		vmci_doorbell_destroy(b->vmci_doorbell);
		b->vmci_doorbell = VMCI_INVALID_HANDLE;
	}
}

/*
 * Initialize vmci doorbell, to get notified as soon as balloon changes
 */
static int vmballoon_vmci_init(struct vmballoon *b)
{
	unsigned long error;

	if ((b->capabilities & VMW_BALLOON_SIGNALLED_WAKEUP_CMD) == 0)
		return 0;

	error = vmci_doorbell_create(&b->vmci_doorbell, VMCI_FLAG_DELAYED_CB,
				     VMCI_PRIVILEGE_FLAG_RESTRICTED,
				     vmballoon_doorbell, b);

	if (error != VMCI_SUCCESS)
		goto fail;

	error =	__vmballoon_cmd(b, VMW_BALLOON_CMD_VMCI_DOORBELL_SET,
				b->vmci_doorbell.context,
				b->vmci_doorbell.resource, NULL);

	if (error != VMW_BALLOON_SUCCESS)
		goto fail;

	return 0;
fail:
	vmballoon_vmci_cleanup(b);
	return -EIO;
}

/*
 * Perform standard reset sequence by popping the balloon (in case it
 * is not  empty) and then restarting protocol. This operation normally
 * happens when host responds with VMW_BALLOON_ERROR_RESET to a command.
 */
static void vmballoon_reset(struct vmballoon *b)
{
	int error;

	down_write(&b->conf_sem);

	vmballoon_vmci_cleanup(b);

	/* free all pages, skipping monitor unlock */
	vmballoon_pop(b);

	if (!vmballoon_send_start(b, VMW_BALLOON_CAPABILITIES))
		return;

	if ((b->capabilities & VMW_BALLOON_BATCHED_CMDS) != 0) {
		if (vmballoon_init_batching(b)) {
			/*
			 * We failed to initialize batching, inform the monitor
			 * about it by sending a null capability.
			 *
			 * The guest will retry in one second.
			 */
			vmballoon_send_start(b, 0);
			return;
		}
	} else if ((b->capabilities & VMW_BALLOON_BASIC_CMDS) != 0) {
		vmballoon_deinit_batching(b);
	}

	b->reset_required = false;

	error = vmballoon_vmci_init(b);
	if (error)
		pr_err("failed to initialize vmci doorbell\n");

	if (!vmballoon_send_guest_id(b))
		pr_err("failed to send guest ID to the host\n");

	up_write(&b->conf_sem);
}

/**
 * vmballoon_work - periodic balloon worker for reset, inflation and deflation.
 *
 * @work: pointer to the &work_struct which is provided by the workqueue.
 *
 * Resets the protocol if needed, gets the new size and adjusts balloon as
 * needed. Repeat in 1 sec.
 */
static void vmballoon_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct vmballoon *b = container_of(dwork, struct vmballoon, dwork);
	int64_t change = 0;

	if (b->reset_required)
		vmballoon_reset(b);

	down_read(&b->conf_sem);

	/*
	 * Update the stats while holding the semaphore to ensure that
	 * @stats_enabled is consistent with whether the stats are actually
	 * enabled
	 */
	vmballoon_stats_gen_inc(b, VMW_BALLOON_STAT_TIMER);

	if (!vmballoon_send_get_target(b))
		change = vmballoon_change(b);

	if (change != 0) {
		pr_debug("%s - size: %u, target %u", __func__,
			 b->size, b->target);

		if (change > 0)
			vmballoon_inflate(b);
		else  /* (change < 0) */
			vmballoon_deflate(b);
	}

	up_read(&b->conf_sem);

	/*
	 * We are using a freezable workqueue so that balloon operations are
	 * stopped while the system transitions to/from sleep/hibernation.
	 */
	queue_delayed_work(system_freezable_wq,
			   dwork, round_jiffies_relative(HZ));

}

/*
 * DEBUGFS Interface
 */
#ifdef CONFIG_DEBUG_FS

static const char * const vmballoon_stat_page_names[] = {
	[VMW_BALLOON_PAGE_STAT_ALLOC]		= "alloc",
	[VMW_BALLOON_PAGE_STAT_ALLOC_FAIL]	= "allocFail",
	[VMW_BALLOON_PAGE_STAT_REFUSED_ALLOC]	= "errAlloc",
	[VMW_BALLOON_PAGE_STAT_REFUSED_FREE]	= "errFree",
	[VMW_BALLOON_PAGE_STAT_FREE]		= "free"
};

static const char * const vmballoon_stat_names[] = {
	[VMW_BALLOON_STAT_TIMER]		= "timer",
	[VMW_BALLOON_STAT_DOORBELL]		= "doorbell"
};

static const char * const vmballoon_page_size_names[] = {
	[VMW_BALLOON_4K_PAGE]			= "4k",
	[VMW_BALLOON_2M_PAGE]			= "2M"
};

static int vmballoon_enable_stats(struct vmballoon *b)
{
	int r = 0;

	down_write(&b->conf_sem);

	/* did we somehow race with another reader which enabled stats? */
	if (b->stats)
		goto out;

	b->stats = kzalloc(sizeof(*b->stats), GFP_KERNEL);

	if (!b->stats) {
		/* allocation failed */
		r = -ENOMEM;
		goto out;
	}
	static_key_enable(&balloon_stat_enabled.key);
out:
	up_write(&b->conf_sem);
	return r;
}

/**
 * vmballoon_debug_show - shows statistics of balloon operations.
 * @f: pointer to the &struct seq_file.
 * @offset: ignored.
 *
 * Provides the statistics that can be accessed in vmmemctl in the debugfs.
 * To avoid the overhead - mainly that of memory - of collecting the statistics,
 * we only collect statistics after the first time the counters are read.
 *
 * Return: zero on success or an error code.
 */
static int vmballoon_debug_show(struct seq_file *f, void *offset)
{
	struct vmballoon *b = f->private;
	int i, j;

	/* enables stats if they are disabled */
	if (!b->stats) {
		int r = vmballoon_enable_stats(b);

		if (r)
			return r;
	}

	/* format capabilities info */
	seq_printf(f, "%-22s: %#4x\n", "balloon capabilities",
		   VMW_BALLOON_CAPABILITIES);
	seq_printf(f, "%-22s: %#4lx\n", "used capabilities",
		   b->capabilities);
	seq_printf(f, "%-22s: %16s\n", "is resetting",
		   b->reset_required ? "y" : "n");

	/* format size info */
	seq_printf(f, "%-22s: %16u\n", "target", b->target);
	seq_printf(f, "%-22s: %16u\n", "current", b->size);

	for (i = 0; i < VMW_BALLOON_CMD_NUM; i++) {
		if (vmballoon_cmd_names[i] == NULL)
			continue;

		seq_printf(f, "%-22s: %16llu (%llu failed)\n",
			   vmballoon_cmd_names[i],
			   atomic64_read(&b->stats->ops[i][VMW_BALLOON_OP_STAT]),
			   atomic64_read(&b->stats->ops[i][VMW_BALLOON_OP_FAIL_STAT]));
	}

	for (i = 0; i < VMW_BALLOON_STAT_NUM; i++)
		seq_printf(f, "%-22s: %16llu\n",
			   vmballoon_stat_names[i],
			   atomic64_read(&b->stats->general_stat[i]));

	for (i = 0; i < VMW_BALLOON_PAGE_STAT_NUM; i++) {
		for (j = 0; j < VMW_BALLOON_NUM_PAGE_SIZES; j++)
			seq_printf(f, "%-18s(%s): %16llu\n",
				   vmballoon_stat_page_names[i],
				   vmballoon_page_size_names[j],
				   atomic64_read(&b->stats->page_stat[i][j]));
	}

	return 0;
}

static int vmballoon_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, vmballoon_debug_show, inode->i_private);
}

static const struct file_operations vmballoon_debug_fops = {
	.owner		= THIS_MODULE,
	.open		= vmballoon_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init vmballoon_debugfs_init(struct vmballoon *b)
{
	int error;

	b->dbg_entry = debugfs_create_file("vmmemctl", S_IRUGO, NULL, b,
					   &vmballoon_debug_fops);
	if (IS_ERR(b->dbg_entry)) {
		error = PTR_ERR(b->dbg_entry);
		pr_err("failed to create debugfs entry, error: %d\n", error);
		return error;
	}

	return 0;
}

static void __exit vmballoon_debugfs_exit(struct vmballoon *b)
{
	static_key_disable(&balloon_stat_enabled.key);
	debugfs_remove(b->dbg_entry);
	kfree(b->stats);
	b->stats = NULL;
}

#else

static inline int vmballoon_debugfs_init(struct vmballoon *b)
{
	return 0;
}

static inline void vmballoon_debugfs_exit(struct vmballoon *b)
{
}

#endif	/* CONFIG_DEBUG_FS */

static int __init vmballoon_init(void)
{
	int error;
	unsigned is_2m_pages;
	/*
	 * Check if we are running on VMware's hypervisor and bail out
	 * if we are not.
	 */
	if (x86_hyper_type != X86_HYPER_VMWARE)
		return -ENODEV;

	for (is_2m_pages = 0; is_2m_pages < VMW_BALLOON_NUM_PAGE_SIZES;
			is_2m_pages++) {
		INIT_LIST_HEAD(&balloon.page_sizes[is_2m_pages].pages);
		INIT_LIST_HEAD(&balloon.page_sizes[is_2m_pages].refused_pages);
	}

	INIT_DELAYED_WORK(&balloon.dwork, vmballoon_work);

	error = vmballoon_debugfs_init(&balloon);
	if (error)
		return error;

	init_rwsem(&balloon.conf_sem);
	balloon.vmci_doorbell = VMCI_INVALID_HANDLE;
	balloon.batch_page = NULL;
	balloon.page = NULL;
	balloon.reset_required = true;

	queue_delayed_work(system_freezable_wq, &balloon.dwork, 0);

	return 0;
}

/*
 * Using late_initcall() instead of module_init() allows the balloon to use the
 * VMCI doorbell even when the balloon is built into the kernel. Otherwise the
 * VMCI is probed only after the balloon is initialized. If the balloon is used
 * as a module, late_initcall() is equivalent to module_init().
 */
late_initcall(vmballoon_init);

static void __exit vmballoon_exit(void)
{
	vmballoon_vmci_cleanup(&balloon);
	cancel_delayed_work_sync(&balloon.dwork);

	vmballoon_debugfs_exit(&balloon);

	/*
	 * Deallocate all reserved memory, and reset connection with monitor.
	 * Reset connection before deallocating memory to avoid potential for
	 * additional spurious resets from guest touching deallocated pages.
	 */
	vmballoon_send_start(&balloon, 0);
	vmballoon_pop(&balloon);
}
module_exit(vmballoon_exit);
