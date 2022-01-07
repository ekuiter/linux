/* SPDX-License-Identifier: GPL-2.0-or-later */
/* General filesystem caching backing cache interface
 *
 * Copyright (C) 2021 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * NOTE!!! See:
 *
 *	Documentation/filesystems/caching/backend-api.rst
 *
 * for a description of the cache backend interface declared here.
 */

#ifndef _LINUX_FSCACHE_CACHE_H
#define _LINUX_FSCACHE_CACHE_H

#include <linux/fscache.h>

enum fscache_cache_trace;
enum fscache_cookie_trace;
enum fscache_access_trace;

enum fscache_cache_state {
	FSCACHE_CACHE_IS_NOT_PRESENT,	/* No cache is present for this name */
	FSCACHE_CACHE_IS_PREPARING,	/* A cache is preparing to come live */
	FSCACHE_CACHE_IS_ACTIVE,	/* Attached cache is active and can be used */
	FSCACHE_CACHE_GOT_IOERROR,	/* Attached cache stopped on I/O error */
	FSCACHE_CACHE_IS_WITHDRAWN,	/* Attached cache is being withdrawn */
#define NR__FSCACHE_CACHE_STATE (FSCACHE_CACHE_IS_WITHDRAWN + 1)
};

/*
 * Cache cookie.
 */
struct fscache_cache {
	const struct fscache_cache_ops *ops;
	struct list_head	cache_link;	/* Link in cache list */
	void			*cache_priv;	/* Private cache data (or NULL) */
	refcount_t		ref;
	atomic_t		n_volumes;	/* Number of active volumes; */
	atomic_t		n_accesses;	/* Number of in-progress accesses on the cache */
	atomic_t		object_count;	/* no. of live objects in this cache */
	unsigned int		debug_id;
	enum fscache_cache_state state;
	char			*name;
};

/*
 * cache operations
 */
struct fscache_cache_ops {
	/* name of cache provider */
	const char *name;

	/* Acquire a volume */
	void (*acquire_volume)(struct fscache_volume *volume);

	/* Free the cache's data attached to a volume */
	void (*free_volume)(struct fscache_volume *volume);

	/* Look up a cookie in the cache */
	bool (*lookup_cookie)(struct fscache_cookie *cookie);

	/* Withdraw an object without any cookie access counts held */
	void (*withdraw_cookie)(struct fscache_cookie *cookie);

	/* Invalidate an object */
	bool (*invalidate_cookie)(struct fscache_cookie *cookie);

	/* Begin an operation for the netfs lib */
	bool (*begin_operation)(struct netfs_cache_resources *cres,
				enum fscache_want_state want_state);

	/* Prepare to write to a live cache object */
	void (*prepare_to_write)(struct fscache_cookie *cookie);
};

extern struct workqueue_struct *fscache_wq;

/*
 * out-of-line cache backend functions
 */
extern struct rw_semaphore fscache_addremove_sem;
extern struct fscache_cache *fscache_acquire_cache(const char *name);
extern void fscache_relinquish_cache(struct fscache_cache *cache);
extern int fscache_add_cache(struct fscache_cache *cache,
			     const struct fscache_cache_ops *ops,
			     void *cache_priv);
extern void fscache_withdraw_cache(struct fscache_cache *cache);
extern void fscache_withdraw_volume(struct fscache_volume *volume);
extern void fscache_withdraw_cookie(struct fscache_cookie *cookie);

extern void fscache_io_error(struct fscache_cache *cache);

extern void fscache_end_volume_access(struct fscache_volume *volume,
				      struct fscache_cookie *cookie,
				      enum fscache_access_trace why);

extern struct fscache_cookie *fscache_get_cookie(struct fscache_cookie *cookie,
						 enum fscache_cookie_trace where);
extern void fscache_put_cookie(struct fscache_cookie *cookie,
			       enum fscache_cookie_trace where);
extern void fscache_end_cookie_access(struct fscache_cookie *cookie,
				      enum fscache_access_trace why);
extern void fscache_cookie_lookup_negative(struct fscache_cookie *cookie);
extern void fscache_resume_after_invalidation(struct fscache_cookie *cookie);
extern void fscache_caching_failed(struct fscache_cookie *cookie);
extern bool fscache_wait_for_operation(struct netfs_cache_resources *cred,
				       enum fscache_want_state state);

/**
 * fscache_cookie_state - Read the state of a cookie
 * @cookie: The cookie to query
 *
 * Get the state of a cookie, imposing an ordering between the cookie contents
 * and the state value.  Paired with fscache_set_cookie_state().
 */
static inline
enum fscache_cookie_state fscache_cookie_state(struct fscache_cookie *cookie)
{
	return smp_load_acquire(&cookie->state);
}

/**
 * fscache_get_key - Get a pointer to the cookie key
 * @cookie: The cookie to query
 *
 * Return a pointer to the where a cookie's key is stored.
 */
static inline void *fscache_get_key(struct fscache_cookie *cookie)
{
	if (cookie->key_len <= sizeof(cookie->inline_key))
		return cookie->inline_key;
	else
		return cookie->key;
}

static inline struct fscache_cookie *fscache_cres_cookie(struct netfs_cache_resources *cres)
{
	return cres->cache_priv;
}

#endif /* _LINUX_FSCACHE_CACHE_H */
