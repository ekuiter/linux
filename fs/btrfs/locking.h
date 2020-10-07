/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 */

#ifndef BTRFS_LOCKING_H
#define BTRFS_LOCKING_H

#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/percpu_counter.h>
#include "extent_io.h"

#define BTRFS_WRITE_LOCK 1
#define BTRFS_READ_LOCK 2
#define BTRFS_WRITE_LOCK_BLOCKING 3
#define BTRFS_READ_LOCK_BLOCKING 4

/*
 * We are limited in number of subclasses by MAX_LOCKDEP_SUBCLASSES, which at
 * the time of this patch is 8, which is how many we use.  Keep this in mind if
 * you decide you want to add another subclass.
 */
enum btrfs_lock_nesting {
	BTRFS_NESTING_NORMAL,

	/*
	 * We are limited to MAX_LOCKDEP_SUBLCLASSES number of subclasses, so
	 * add this in here and add a static_assert to keep us from going over
	 * the limit.  As of this writing we're limited to 8, and we're
	 * definitely using 8, hence this check to keep us from messing up in
	 * the future.
	 */
	BTRFS_NESTING_MAX,
};

static_assert(BTRFS_NESTING_MAX <= MAX_LOCKDEP_SUBCLASSES,
	      "too many lock subclasses defined");

struct btrfs_path;

void __btrfs_tree_lock(struct extent_buffer *eb, enum btrfs_lock_nesting nest);
void btrfs_tree_lock(struct extent_buffer *eb);
void btrfs_tree_unlock(struct extent_buffer *eb);

void __btrfs_tree_read_lock(struct extent_buffer *eb, enum btrfs_lock_nesting nest,
			    bool recurse);
void btrfs_tree_read_lock(struct extent_buffer *eb);
void btrfs_tree_read_unlock(struct extent_buffer *eb);
void btrfs_tree_read_unlock_blocking(struct extent_buffer *eb);
void btrfs_set_lock_blocking_read(struct extent_buffer *eb);
void btrfs_set_lock_blocking_write(struct extent_buffer *eb);
int btrfs_try_tree_read_lock(struct extent_buffer *eb);
int btrfs_try_tree_write_lock(struct extent_buffer *eb);
int btrfs_tree_read_lock_atomic(struct extent_buffer *eb);
struct extent_buffer *btrfs_lock_root_node(struct btrfs_root *root);
struct extent_buffer *__btrfs_read_lock_root_node(struct btrfs_root *root,
						  bool recurse);

static inline struct extent_buffer *btrfs_read_lock_root_node(struct btrfs_root *root)
{
	return __btrfs_read_lock_root_node(root, false);
}

#ifdef CONFIG_BTRFS_DEBUG
static inline void btrfs_assert_tree_locked(struct extent_buffer *eb) {
	BUG_ON(!eb->write_locks);
}
#else
static inline void btrfs_assert_tree_locked(struct extent_buffer *eb) { }
#endif

void btrfs_set_path_blocking(struct btrfs_path *p);
void btrfs_unlock_up_safe(struct btrfs_path *path, int level);

static inline void btrfs_tree_unlock_rw(struct extent_buffer *eb, int rw)
{
	if (rw == BTRFS_WRITE_LOCK || rw == BTRFS_WRITE_LOCK_BLOCKING)
		btrfs_tree_unlock(eb);
	else if (rw == BTRFS_READ_LOCK_BLOCKING)
		btrfs_tree_read_unlock_blocking(eb);
	else if (rw == BTRFS_READ_LOCK)
		btrfs_tree_read_unlock(eb);
	else
		BUG();
}

struct btrfs_drew_lock {
	atomic_t readers;
	struct percpu_counter writers;
	wait_queue_head_t pending_writers;
	wait_queue_head_t pending_readers;
};

int btrfs_drew_lock_init(struct btrfs_drew_lock *lock);
void btrfs_drew_lock_destroy(struct btrfs_drew_lock *lock);
void btrfs_drew_write_lock(struct btrfs_drew_lock *lock);
bool btrfs_drew_try_write_lock(struct btrfs_drew_lock *lock);
void btrfs_drew_write_unlock(struct btrfs_drew_lock *lock);
void btrfs_drew_read_lock(struct btrfs_drew_lock *lock);
void btrfs_drew_read_unlock(struct btrfs_drew_lock *lock);

#endif
