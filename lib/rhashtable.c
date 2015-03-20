/*
 * Resizable, Scalable, Concurrent Hash Table
 *
 * Copyright (c) 2015 Herbert Xu <herbert@gondor.apana.org.au>
 * Copyright (c) 2014-2015 Thomas Graf <tgraf@suug.ch>
 * Copyright (c) 2008-2014 Patrick McHardy <kaber@trash.net>
 *
 * Code partially derived from nft_hash
 * Rewritten with rehash code from br_multicast plus single list
 * pointer as suggested by Josh Triplett
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/rhashtable.h>
#include <linux/err.h>

#define HASH_DEFAULT_SIZE	64UL
#define HASH_MIN_SIZE		4U
#define BUCKET_LOCKS_PER_CPU   128UL

static u32 head_hashfn(struct rhashtable *ht,
		       const struct bucket_table *tbl,
		       const struct rhash_head *he)
{
	return rht_head_hashfn(ht, tbl, he, ht->p);
}

#ifdef CONFIG_PROVE_LOCKING
#define ASSERT_RHT_MUTEX(HT) BUG_ON(!lockdep_rht_mutex_is_held(HT))

int lockdep_rht_mutex_is_held(struct rhashtable *ht)
{
	return (debug_locks) ? lockdep_is_held(&ht->mutex) : 1;
}
EXPORT_SYMBOL_GPL(lockdep_rht_mutex_is_held);

int lockdep_rht_bucket_is_held(const struct bucket_table *tbl, u32 hash)
{
	spinlock_t *lock = rht_bucket_lock(tbl, hash);

	return (debug_locks) ? lockdep_is_held(lock) : 1;
}
EXPORT_SYMBOL_GPL(lockdep_rht_bucket_is_held);
#else
#define ASSERT_RHT_MUTEX(HT)
#endif


static int alloc_bucket_locks(struct rhashtable *ht, struct bucket_table *tbl)
{
	unsigned int i, size;
#if defined(CONFIG_PROVE_LOCKING)
	unsigned int nr_pcpus = 2;
#else
	unsigned int nr_pcpus = num_possible_cpus();
#endif

	nr_pcpus = min_t(unsigned int, nr_pcpus, 32UL);
	size = roundup_pow_of_two(nr_pcpus * ht->p.locks_mul);

	/* Never allocate more than 0.5 locks per bucket */
	size = min_t(unsigned int, size, tbl->size >> 1);

	if (sizeof(spinlock_t) != 0) {
#ifdef CONFIG_NUMA
		if (size * sizeof(spinlock_t) > PAGE_SIZE)
			tbl->locks = vmalloc(size * sizeof(spinlock_t));
		else
#endif
		tbl->locks = kmalloc_array(size, sizeof(spinlock_t),
					   GFP_KERNEL);
		if (!tbl->locks)
			return -ENOMEM;
		for (i = 0; i < size; i++)
			spin_lock_init(&tbl->locks[i]);
	}
	tbl->locks_mask = size - 1;

	return 0;
}

static void bucket_table_free(const struct bucket_table *tbl)
{
	if (tbl)
		kvfree(tbl->locks);

	kvfree(tbl);
}

static void bucket_table_free_rcu(struct rcu_head *head)
{
	bucket_table_free(container_of(head, struct bucket_table, rcu));
}

static struct bucket_table *bucket_table_alloc(struct rhashtable *ht,
					       size_t nbuckets)
{
	struct bucket_table *tbl = NULL;
	size_t size;
	int i;

	size = sizeof(*tbl) + nbuckets * sizeof(tbl->buckets[0]);
	if (size <= (PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER))
		tbl = kzalloc(size, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
	if (tbl == NULL)
		tbl = vzalloc(size);
	if (tbl == NULL)
		return NULL;

	tbl->size = nbuckets;

	if (alloc_bucket_locks(ht, tbl) < 0) {
		bucket_table_free(tbl);
		return NULL;
	}

	INIT_LIST_HEAD(&tbl->walkers);

	get_random_bytes(&tbl->hash_rnd, sizeof(tbl->hash_rnd));

	for (i = 0; i < nbuckets; i++)
		INIT_RHT_NULLS_HEAD(tbl->buckets[i], ht, i);

	return tbl;
}

static int rhashtable_rehash_one(struct rhashtable *ht, unsigned old_hash)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	struct bucket_table *new_tbl =
		rht_dereference(old_tbl->future_tbl, ht) ?: old_tbl;
	struct rhash_head __rcu **pprev = &old_tbl->buckets[old_hash];
	int err = -ENOENT;
	struct rhash_head *head, *next, *entry;
	spinlock_t *new_bucket_lock;
	unsigned new_hash;

	rht_for_each(entry, old_tbl, old_hash) {
		err = 0;
		next = rht_dereference_bucket(entry->next, old_tbl, old_hash);

		if (rht_is_a_nulls(next))
			break;

		pprev = &entry->next;
	}

	if (err)
		goto out;

	new_hash = head_hashfn(ht, new_tbl, entry);

	new_bucket_lock = rht_bucket_lock(new_tbl, new_hash);

	spin_lock_nested(new_bucket_lock, SINGLE_DEPTH_NESTING);
	head = rht_dereference_bucket(new_tbl->buckets[new_hash],
				      new_tbl, new_hash);

	if (rht_is_a_nulls(head))
		INIT_RHT_NULLS_HEAD(entry->next, ht, new_hash);
	else
		RCU_INIT_POINTER(entry->next, head);

	rcu_assign_pointer(new_tbl->buckets[new_hash], entry);
	spin_unlock(new_bucket_lock);

	rcu_assign_pointer(*pprev, next);

out:
	return err;
}

static void rhashtable_rehash_chain(struct rhashtable *ht, unsigned old_hash)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	spinlock_t *old_bucket_lock;

	old_bucket_lock = rht_bucket_lock(old_tbl, old_hash);

	spin_lock_bh(old_bucket_lock);
	while (!rhashtable_rehash_one(ht, old_hash))
		;
	old_tbl->rehash++;
	spin_unlock_bh(old_bucket_lock);
}

static void rhashtable_rehash(struct rhashtable *ht,
			      struct bucket_table *new_tbl)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	struct rhashtable_walker *walker;
	unsigned old_hash;

	/* Make insertions go into the new, empty table right away. Deletions
	 * and lookups will be attempted in both tables until we synchronize.
	 */
	rcu_assign_pointer(old_tbl->future_tbl, new_tbl);

	/* Ensure the new table is visible to readers. */
	smp_wmb();

	for (old_hash = 0; old_hash < old_tbl->size; old_hash++)
		rhashtable_rehash_chain(ht, old_hash);

	/* Publish the new table pointer. */
	rcu_assign_pointer(ht->tbl, new_tbl);

	list_for_each_entry(walker, &old_tbl->walkers, list)
		walker->tbl = NULL;

	/* Wait for readers. All new readers will see the new
	 * table, and thus no references to the old table will
	 * remain.
	 */
	call_rcu(&old_tbl->rcu, bucket_table_free_rcu);
}

/**
 * rhashtable_expand - Expand hash table while allowing concurrent lookups
 * @ht:		the hash table to expand
 *
 * A secondary bucket array is allocated and the hash entries are migrated.
 *
 * This function may only be called in a context where it is safe to call
 * synchronize_rcu(), e.g. not within a rcu_read_lock() section.
 *
 * The caller must ensure that no concurrent resizing occurs by holding
 * ht->mutex.
 *
 * It is valid to have concurrent insertions and deletions protected by per
 * bucket locks or concurrent RCU protected lookups and traversals.
 */
int rhashtable_expand(struct rhashtable *ht)
{
	struct bucket_table *new_tbl, *old_tbl = rht_dereference(ht->tbl, ht);

	ASSERT_RHT_MUTEX(ht);

	new_tbl = bucket_table_alloc(ht, old_tbl->size * 2);
	if (new_tbl == NULL)
		return -ENOMEM;

	rhashtable_rehash(ht, new_tbl);
	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_expand);

/**
 * rhashtable_shrink - Shrink hash table while allowing concurrent lookups
 * @ht:		the hash table to shrink
 *
 * This function may only be called in a context where it is safe to call
 * synchronize_rcu(), e.g. not within a rcu_read_lock() section.
 *
 * The caller must ensure that no concurrent resizing occurs by holding
 * ht->mutex.
 *
 * The caller must ensure that no concurrent table mutations take place.
 * It is however valid to have concurrent lookups if they are RCU protected.
 *
 * It is valid to have concurrent insertions and deletions protected by per
 * bucket locks or concurrent RCU protected lookups and traversals.
 */
int rhashtable_shrink(struct rhashtable *ht)
{
	struct bucket_table *new_tbl, *old_tbl = rht_dereference(ht->tbl, ht);

	ASSERT_RHT_MUTEX(ht);

	new_tbl = bucket_table_alloc(ht, old_tbl->size / 2);
	if (new_tbl == NULL)
		return -ENOMEM;

	rhashtable_rehash(ht, new_tbl);
	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_shrink);

static void rht_deferred_worker(struct work_struct *work)
{
	struct rhashtable *ht;
	struct bucket_table *tbl;

	ht = container_of(work, struct rhashtable, run_work);
	mutex_lock(&ht->mutex);
	if (ht->being_destroyed)
		goto unlock;

	tbl = rht_dereference(ht->tbl, ht);

	if (rht_grow_above_75(ht, tbl))
		rhashtable_expand(ht);
	else if (rht_shrink_below_30(ht, tbl))
		rhashtable_shrink(ht);
unlock:
	mutex_unlock(&ht->mutex);
}

int rhashtable_insert_slow(struct rhashtable *ht, const void *key,
			   struct rhash_head *obj,
			   struct bucket_table *tbl)
{
	struct rhash_head *head;
	unsigned hash;
	int err = -EEXIST;

	hash = head_hashfn(ht, tbl, obj);
	spin_lock_nested(rht_bucket_lock(tbl, hash), SINGLE_DEPTH_NESTING);

	if (key && rhashtable_lookup_fast(ht, key, ht->p))
		goto exit;

	err = 0;

	head = rht_dereference_bucket(tbl->buckets[hash], tbl, hash);

	RCU_INIT_POINTER(obj->next, head);

	rcu_assign_pointer(tbl->buckets[hash], obj);

	atomic_inc(&ht->nelems);

exit:
	spin_unlock(rht_bucket_lock(tbl, hash));

	return err;
}
EXPORT_SYMBOL_GPL(rhashtable_insert_slow);

/**
 * rhashtable_walk_init - Initialise an iterator
 * @ht:		Table to walk over
 * @iter:	Hash table Iterator
 *
 * This function prepares a hash table walk.
 *
 * Note that if you restart a walk after rhashtable_walk_stop you
 * may see the same object twice.  Also, you may miss objects if
 * there are removals in between rhashtable_walk_stop and the next
 * call to rhashtable_walk_start.
 *
 * For a completely stable walk you should construct your own data
 * structure outside the hash table.
 *
 * This function may sleep so you must not call it from interrupt
 * context or with spin locks held.
 *
 * You must call rhashtable_walk_exit if this function returns
 * successfully.
 */
int rhashtable_walk_init(struct rhashtable *ht, struct rhashtable_iter *iter)
{
	iter->ht = ht;
	iter->p = NULL;
	iter->slot = 0;
	iter->skip = 0;

	iter->walker = kmalloc(sizeof(*iter->walker), GFP_KERNEL);
	if (!iter->walker)
		return -ENOMEM;

	mutex_lock(&ht->mutex);
	iter->walker->tbl = rht_dereference(ht->tbl, ht);
	list_add(&iter->walker->list, &iter->walker->tbl->walkers);
	mutex_unlock(&ht->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_walk_init);

/**
 * rhashtable_walk_exit - Free an iterator
 * @iter:	Hash table Iterator
 *
 * This function frees resources allocated by rhashtable_walk_init.
 */
void rhashtable_walk_exit(struct rhashtable_iter *iter)
{
	mutex_lock(&iter->ht->mutex);
	if (iter->walker->tbl)
		list_del(&iter->walker->list);
	mutex_unlock(&iter->ht->mutex);
	kfree(iter->walker);
}
EXPORT_SYMBOL_GPL(rhashtable_walk_exit);

/**
 * rhashtable_walk_start - Start a hash table walk
 * @iter:	Hash table iterator
 *
 * Start a hash table walk.  Note that we take the RCU lock in all
 * cases including when we return an error.  So you must always call
 * rhashtable_walk_stop to clean up.
 *
 * Returns zero if successful.
 *
 * Returns -EAGAIN if resize event occured.  Note that the iterator
 * will rewind back to the beginning and you may use it immediately
 * by calling rhashtable_walk_next.
 */
int rhashtable_walk_start(struct rhashtable_iter *iter)
	__acquires(RCU)
{
	struct rhashtable *ht = iter->ht;

	mutex_lock(&ht->mutex);

	if (iter->walker->tbl)
		list_del(&iter->walker->list);

	rcu_read_lock();

	mutex_unlock(&ht->mutex);

	if (!iter->walker->tbl) {
		iter->walker->tbl = rht_dereference_rcu(ht->tbl, ht);
		return -EAGAIN;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_walk_start);

/**
 * rhashtable_walk_next - Return the next object and advance the iterator
 * @iter:	Hash table iterator
 *
 * Note that you must call rhashtable_walk_stop when you are finished
 * with the walk.
 *
 * Returns the next object or NULL when the end of the table is reached.
 *
 * Returns -EAGAIN if resize event occured.  Note that the iterator
 * will rewind back to the beginning and you may continue to use it.
 */
void *rhashtable_walk_next(struct rhashtable_iter *iter)
{
	struct bucket_table *tbl = iter->walker->tbl;
	struct rhashtable *ht = iter->ht;
	struct rhash_head *p = iter->p;
	void *obj = NULL;

	if (p) {
		p = rht_dereference_bucket_rcu(p->next, tbl, iter->slot);
		goto next;
	}

	for (; iter->slot < tbl->size; iter->slot++) {
		int skip = iter->skip;

		rht_for_each_rcu(p, tbl, iter->slot) {
			if (!skip)
				break;
			skip--;
		}

next:
		if (!rht_is_a_nulls(p)) {
			iter->skip++;
			iter->p = p;
			obj = rht_obj(ht, p);
			goto out;
		}

		iter->skip = 0;
	}

	iter->walker->tbl = rht_dereference_rcu(tbl->future_tbl, ht);
	if (iter->walker->tbl) {
		iter->slot = 0;
		iter->skip = 0;
		return ERR_PTR(-EAGAIN);
	}

	iter->p = NULL;

out:

	return obj;
}
EXPORT_SYMBOL_GPL(rhashtable_walk_next);

/**
 * rhashtable_walk_stop - Finish a hash table walk
 * @iter:	Hash table iterator
 *
 * Finish a hash table walk.
 */
void rhashtable_walk_stop(struct rhashtable_iter *iter)
	__releases(RCU)
{
	struct rhashtable *ht;
	struct bucket_table *tbl = iter->walker->tbl;

	if (!tbl)
		goto out;

	ht = iter->ht;

	mutex_lock(&ht->mutex);
	if (tbl->rehash < tbl->size)
		list_add(&iter->walker->list, &tbl->walkers);
	else
		iter->walker->tbl = NULL;
	mutex_unlock(&ht->mutex);

	iter->p = NULL;

out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(rhashtable_walk_stop);

static size_t rounded_hashtable_size(const struct rhashtable_params *params)
{
	return max(roundup_pow_of_two(params->nelem_hint * 4 / 3),
		   (unsigned long)params->min_size);
}

/**
 * rhashtable_init - initialize a new hash table
 * @ht:		hash table to be initialized
 * @params:	configuration parameters
 *
 * Initializes a new hash table based on the provided configuration
 * parameters. A table can be configured either with a variable or
 * fixed length key:
 *
 * Configuration Example 1: Fixed length keys
 * struct test_obj {
 *	int			key;
 *	void *			my_member;
 *	struct rhash_head	node;
 * };
 *
 * struct rhashtable_params params = {
 *	.head_offset = offsetof(struct test_obj, node),
 *	.key_offset = offsetof(struct test_obj, key),
 *	.key_len = sizeof(int),
 *	.hashfn = jhash,
 *	.nulls_base = (1U << RHT_BASE_SHIFT),
 * };
 *
 * Configuration Example 2: Variable length keys
 * struct test_obj {
 *	[...]
 *	struct rhash_head	node;
 * };
 *
 * u32 my_hash_fn(const void *data, u32 seed)
 * {
 *	struct test_obj *obj = data;
 *
 *	return [... hash ...];
 * }
 *
 * struct rhashtable_params params = {
 *	.head_offset = offsetof(struct test_obj, node),
 *	.hashfn = jhash,
 *	.obj_hashfn = my_hash_fn,
 * };
 */
int rhashtable_init(struct rhashtable *ht,
		    const struct rhashtable_params *params)
{
	struct bucket_table *tbl;
	size_t size;

	size = HASH_DEFAULT_SIZE;

	if ((!(params->key_len && params->hashfn) && !params->obj_hashfn) ||
	    (params->obj_hashfn && !params->obj_cmpfn))
		return -EINVAL;

	if (params->nulls_base && params->nulls_base < (1U << RHT_BASE_SHIFT))
		return -EINVAL;

	if (params->nelem_hint)
		size = rounded_hashtable_size(params);

	memset(ht, 0, sizeof(*ht));
	mutex_init(&ht->mutex);
	memcpy(&ht->p, params, sizeof(*params));

	if (params->min_size)
		ht->p.min_size = roundup_pow_of_two(params->min_size);

	if (params->max_size)
		ht->p.max_size = rounddown_pow_of_two(params->max_size);

	ht->p.min_size = max(ht->p.min_size, HASH_MIN_SIZE);

	if (params->locks_mul)
		ht->p.locks_mul = roundup_pow_of_two(params->locks_mul);
	else
		ht->p.locks_mul = BUCKET_LOCKS_PER_CPU;

	tbl = bucket_table_alloc(ht, size);
	if (tbl == NULL)
		return -ENOMEM;

	atomic_set(&ht->nelems, 0);

	RCU_INIT_POINTER(ht->tbl, tbl);

	INIT_WORK(&ht->run_work, rht_deferred_worker);

	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_init);

/**
 * rhashtable_destroy - destroy hash table
 * @ht:		the hash table to destroy
 *
 * Frees the bucket array. This function is not rcu safe, therefore the caller
 * has to make sure that no resizing may happen by unpublishing the hashtable
 * and waiting for the quiescent cycle before releasing the bucket array.
 */
void rhashtable_destroy(struct rhashtable *ht)
{
	ht->being_destroyed = true;

	cancel_work_sync(&ht->run_work);

	mutex_lock(&ht->mutex);
	bucket_table_free(rht_dereference(ht->tbl, ht));
	mutex_unlock(&ht->mutex);
}
EXPORT_SYMBOL_GPL(rhashtable_destroy);
