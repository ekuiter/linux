/*
 * include/linux/backing-dev.h
 *
 * low-level device information and state which is propagated up through
 * to high-level code.
 */

#ifndef _LINUX_BACKING_DEV_H
#define _LINUX_BACKING_DEV_H

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>
#include <linux/blk-cgroup.h>
#include <linux/backing-dev-defs.h>

int __must_check bdi_init(struct backing_dev_info *bdi);
void bdi_destroy(struct backing_dev_info *bdi);

__printf(3, 4)
int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...);
int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev);
void bdi_unregister(struct backing_dev_info *bdi);
int __must_check bdi_setup_and_register(struct backing_dev_info *, char *);
void bdi_start_writeback(struct backing_dev_info *bdi, long nr_pages,
			enum wb_reason reason);
void bdi_start_background_writeback(struct backing_dev_info *bdi);
void wb_workfn(struct work_struct *work);
int bdi_has_dirty_io(struct backing_dev_info *bdi);
void wb_wakeup_delayed(struct bdi_writeback *wb);

extern spinlock_t bdi_lock;
extern struct list_head bdi_list;

extern struct workqueue_struct *bdi_wq;

static inline int wb_has_dirty_io(struct bdi_writeback *wb)
{
	return !list_empty(&wb->b_dirty) ||
	       !list_empty(&wb->b_io) ||
	       !list_empty(&wb->b_more_io);
}

static inline void __add_wb_stat(struct bdi_writeback *wb,
				 enum wb_stat_item item, s64 amount)
{
	__percpu_counter_add(&wb->stat[item], amount, WB_STAT_BATCH);
}

static inline void __inc_wb_stat(struct bdi_writeback *wb,
				 enum wb_stat_item item)
{
	__add_wb_stat(wb, item, 1);
}

static inline void inc_wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__inc_wb_stat(wb, item);
	local_irq_restore(flags);
}

static inline void __dec_wb_stat(struct bdi_writeback *wb,
				 enum wb_stat_item item)
{
	__add_wb_stat(wb, item, -1);
}

static inline void dec_wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__dec_wb_stat(wb, item);
	local_irq_restore(flags);
}

static inline s64 wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	return percpu_counter_read_positive(&wb->stat[item]);
}

static inline s64 __wb_stat_sum(struct bdi_writeback *wb,
				enum wb_stat_item item)
{
	return percpu_counter_sum_positive(&wb->stat[item]);
}

static inline s64 wb_stat_sum(struct bdi_writeback *wb, enum wb_stat_item item)
{
	s64 sum;
	unsigned long flags;

	local_irq_save(flags);
	sum = __wb_stat_sum(wb, item);
	local_irq_restore(flags);

	return sum;
}

extern void wb_writeout_inc(struct bdi_writeback *wb);

/*
 * maximal error of a stat counter.
 */
static inline unsigned long wb_stat_error(struct bdi_writeback *wb)
{
#ifdef CONFIG_SMP
	return nr_cpu_ids * WB_STAT_BATCH;
#else
	return 1;
#endif
}

int bdi_set_min_ratio(struct backing_dev_info *bdi, unsigned int min_ratio);
int bdi_set_max_ratio(struct backing_dev_info *bdi, unsigned int max_ratio);

/*
 * Flags in backing_dev_info::capability
 *
 * The first three flags control whether dirty pages will contribute to the
 * VM's accounting and whether writepages() should be called for dirty pages
 * (something that would not, for example, be appropriate for ramfs)
 *
 * WARNING: these flags are closely related and should not normally be
 * used separately.  The BDI_CAP_NO_ACCT_AND_WRITEBACK combines these
 * three flags into a single convenience macro.
 *
 * BDI_CAP_NO_ACCT_DIRTY:  Dirty pages shouldn't contribute to accounting
 * BDI_CAP_NO_WRITEBACK:   Don't write pages back
 * BDI_CAP_NO_ACCT_WB:     Don't automatically account writeback pages
 * BDI_CAP_STRICTLIMIT:    Keep number of dirty pages below bdi threshold.
 *
 * BDI_CAP_CGROUP_WRITEBACK: Supports cgroup-aware writeback.
 */
#define BDI_CAP_NO_ACCT_DIRTY	0x00000001
#define BDI_CAP_NO_WRITEBACK	0x00000002
#define BDI_CAP_NO_ACCT_WB	0x00000004
#define BDI_CAP_STABLE_WRITES	0x00000008
#define BDI_CAP_STRICTLIMIT	0x00000010
#define BDI_CAP_CGROUP_WRITEBACK 0x00000020

#define BDI_CAP_NO_ACCT_AND_WRITEBACK \
	(BDI_CAP_NO_WRITEBACK | BDI_CAP_NO_ACCT_DIRTY | BDI_CAP_NO_ACCT_WB)

extern struct backing_dev_info noop_backing_dev_info;

int writeback_in_progress(struct backing_dev_info *bdi);

static inline struct backing_dev_info *inode_to_bdi(struct inode *inode)
{
	struct super_block *sb;

	if (!inode)
		return &noop_backing_dev_info;

	sb = inode->i_sb;
#ifdef CONFIG_BLOCK
	if (sb_is_blkdev_sb(sb))
		return blk_get_backing_dev_info(I_BDEV(inode));
#endif
	return sb->s_bdi;
}

static inline int wb_congested(struct bdi_writeback *wb, int cong_bits)
{
	struct backing_dev_info *bdi = wb->bdi;

	if (bdi->congested_fn)
		return bdi->congested_fn(bdi->congested_data, cong_bits);
	return wb->congested->state & cong_bits;
}

long congestion_wait(int sync, long timeout);
long wait_iff_congested(struct zone *zone, int sync, long timeout);
int pdflush_proc_obsolete(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos);

static inline bool bdi_cap_stable_pages_required(struct backing_dev_info *bdi)
{
	return bdi->capabilities & BDI_CAP_STABLE_WRITES;
}

static inline bool bdi_cap_writeback_dirty(struct backing_dev_info *bdi)
{
	return !(bdi->capabilities & BDI_CAP_NO_WRITEBACK);
}

static inline bool bdi_cap_account_dirty(struct backing_dev_info *bdi)
{
	return !(bdi->capabilities & BDI_CAP_NO_ACCT_DIRTY);
}

static inline bool bdi_cap_account_writeback(struct backing_dev_info *bdi)
{
	/* Paranoia: BDI_CAP_NO_WRITEBACK implies BDI_CAP_NO_ACCT_WB */
	return !(bdi->capabilities & (BDI_CAP_NO_ACCT_WB |
				      BDI_CAP_NO_WRITEBACK));
}

static inline bool mapping_cap_writeback_dirty(struct address_space *mapping)
{
	return bdi_cap_writeback_dirty(inode_to_bdi(mapping->host));
}

static inline bool mapping_cap_account_dirty(struct address_space *mapping)
{
	return bdi_cap_account_dirty(inode_to_bdi(mapping->host));
}

static inline int bdi_sched_wait(void *word)
{
	schedule();
	return 0;
}

#ifdef CONFIG_CGROUP_WRITEBACK

struct bdi_writeback_congested *
wb_congested_get_create(struct backing_dev_info *bdi, int blkcg_id, gfp_t gfp);
void wb_congested_put(struct bdi_writeback_congested *congested);
struct bdi_writeback *wb_get_create(struct backing_dev_info *bdi,
				    struct cgroup_subsys_state *memcg_css,
				    gfp_t gfp);
void __inode_attach_wb(struct inode *inode, struct page *page);
void wb_memcg_offline(struct mem_cgroup *memcg);
void wb_blkcg_offline(struct blkcg *blkcg);

/**
 * inode_cgwb_enabled - test whether cgroup writeback is enabled on an inode
 * @inode: inode of interest
 *
 * cgroup writeback requires support from both the bdi and filesystem.
 * Test whether @inode has both.
 */
static inline bool inode_cgwb_enabled(struct inode *inode)
{
	struct backing_dev_info *bdi = inode_to_bdi(inode);

	return bdi_cap_account_dirty(bdi) &&
		(bdi->capabilities & BDI_CAP_CGROUP_WRITEBACK) &&
		(inode->i_sb->s_type->fs_flags & FS_CGROUP_WRITEBACK);
}

/**
 * wb_tryget - try to increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline bool wb_tryget(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		return percpu_ref_tryget(&wb->refcnt);
	return true;
}

/**
 * wb_get - increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline void wb_get(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		percpu_ref_get(&wb->refcnt);
}

/**
 * wb_put - decrement a wb's refcount
 * @wb: bdi_writeback to put
 */
static inline void wb_put(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		percpu_ref_put(&wb->refcnt);
}

/**
 * wb_find_current - find wb for %current on a bdi
 * @bdi: bdi of interest
 *
 * Find the wb of @bdi which matches both the memcg and blkcg of %current.
 * Must be called under rcu_read_lock() which protects the returend wb.
 * NULL if not found.
 */
static inline struct bdi_writeback *wb_find_current(struct backing_dev_info *bdi)
{
	struct cgroup_subsys_state *memcg_css;
	struct bdi_writeback *wb;

	memcg_css = task_css(current, memory_cgrp_id);
	if (!memcg_css->parent)
		return &bdi->wb;

	wb = radix_tree_lookup(&bdi->cgwb_tree, memcg_css->id);

	/*
	 * %current's blkcg equals the effective blkcg of its memcg.  No
	 * need to use the relatively expensive cgroup_get_e_css().
	 */
	if (likely(wb && wb->blkcg_css == task_css(current, blkio_cgrp_id)))
		return wb;
	return NULL;
}

/**
 * wb_get_create_current - get or create wb for %current on a bdi
 * @bdi: bdi of interest
 * @gfp: allocation mask
 *
 * Equivalent to wb_get_create() on %current's memcg.  This function is
 * called from a relatively hot path and optimizes the common cases using
 * wb_find_current().
 */
static inline struct bdi_writeback *
wb_get_create_current(struct backing_dev_info *bdi, gfp_t gfp)
{
	struct bdi_writeback *wb;

	rcu_read_lock();
	wb = wb_find_current(bdi);
	if (wb && unlikely(!wb_tryget(wb)))
		wb = NULL;
	rcu_read_unlock();

	if (unlikely(!wb)) {
		struct cgroup_subsys_state *memcg_css;

		memcg_css = task_get_css(current, memory_cgrp_id);
		wb = wb_get_create(bdi, memcg_css, gfp);
		css_put(memcg_css);
	}
	return wb;
}

/**
 * inode_attach_wb - associate an inode with its wb
 * @inode: inode of interest
 * @page: page being dirtied (may be NULL)
 *
 * If @inode doesn't have its wb, associate it with the wb matching the
 * memcg of @page or, if @page is NULL, %current.  May be called w/ or w/o
 * @inode->i_lock.
 */
static inline void inode_attach_wb(struct inode *inode, struct page *page)
{
	if (!inode->i_wb)
		__inode_attach_wb(inode, page);
}

/**
 * inode_detach_wb - disassociate an inode from its wb
 * @inode: inode of interest
 *
 * @inode is being freed.  Detach from its wb.
 */
static inline void inode_detach_wb(struct inode *inode)
{
	if (inode->i_wb) {
		wb_put(inode->i_wb);
		inode->i_wb = NULL;
	}
}

/**
 * inode_to_wb - determine the wb of an inode
 * @inode: inode of interest
 *
 * Returns the wb @inode is currently associated with.
 */
static inline struct bdi_writeback *inode_to_wb(struct inode *inode)
{
	return inode->i_wb;
}

#else	/* CONFIG_CGROUP_WRITEBACK */

static inline bool inode_cgwb_enabled(struct inode *inode)
{
	return false;
}

static inline struct bdi_writeback_congested *
wb_congested_get_create(struct backing_dev_info *bdi, int blkcg_id, gfp_t gfp)
{
	return bdi->wb.congested;
}

static inline void wb_congested_put(struct bdi_writeback_congested *congested)
{
}

static inline bool wb_tryget(struct bdi_writeback *wb)
{
	return true;
}

static inline void wb_get(struct bdi_writeback *wb)
{
}

static inline void wb_put(struct bdi_writeback *wb)
{
}

static inline struct bdi_writeback *wb_find_current(struct backing_dev_info *bdi)
{
	return &bdi->wb;
}

static inline struct bdi_writeback *
wb_get_create_current(struct backing_dev_info *bdi, gfp_t gfp)
{
	return &bdi->wb;
}

static inline void inode_attach_wb(struct inode *inode, struct page *page)
{
}

static inline void inode_detach_wb(struct inode *inode)
{
}

static inline struct bdi_writeback *inode_to_wb(struct inode *inode)
{
	return &inode_to_bdi(inode)->wb;
}

static inline void wb_memcg_offline(struct mem_cgroup *memcg)
{
}

static inline void wb_blkcg_offline(struct blkcg *blkcg)
{
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

static inline int bdi_congested(struct backing_dev_info *bdi, int cong_bits)
{
	return wb_congested(&bdi->wb, cong_bits);
}

static inline int bdi_read_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, 1 << WB_sync_congested);
}

static inline int bdi_write_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, 1 << WB_async_congested);
}

static inline int bdi_rw_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, (1 << WB_sync_congested) |
				  (1 << WB_async_congested));
}

#endif	/* _LINUX_BACKING_DEV_H */
