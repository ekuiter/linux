/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Christian König <christian.koenig@amd.com>
 */

#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_trace.h"

struct amdgpu_sync_entry {
	struct hlist_node	node;
	struct fence		*fence;
};

/**
 * amdgpu_sync_create - zero init sync object
 *
 * @sync: sync object to initialize
 *
 * Just clear the sync object for now.
 */
void amdgpu_sync_create(struct amdgpu_sync *sync)
{
	unsigned i;

	for (i = 0; i < AMDGPU_MAX_RINGS; ++i)
		sync->sync_to[i] = NULL;

	hash_init(sync->fences);
	sync->last_vm_update = NULL;
}

static bool amdgpu_sync_same_dev(struct amdgpu_device *adev, struct fence *f)
{
	struct amdgpu_fence *a_fence = to_amdgpu_fence(f);
	struct amd_sched_fence *s_fence = to_amd_sched_fence(f);

	if (a_fence)
		return a_fence->ring->adev == adev;

	if (s_fence) {
		struct amdgpu_ring *ring;

		ring = container_of(s_fence->sched, struct amdgpu_ring, sched);
		return ring->adev == adev;
	}

	return false;
}

static bool amdgpu_sync_test_owner(struct fence *f, void *owner)
{
	struct amdgpu_fence *a_fence = to_amdgpu_fence(f);
	struct amd_sched_fence *s_fence = to_amd_sched_fence(f);
	if (s_fence)
		return s_fence->owner == owner;
	if (a_fence)
		return a_fence->owner == owner;
	return false;
}

static void amdgpu_sync_keep_later(struct fence **keep, struct fence *fence)
{
	if (*keep && fence_is_later(*keep, fence))
		return;

	fence_put(*keep);
	*keep = fence_get(fence);
}

/**
 * amdgpu_sync_fence - remember to sync to this fence
 *
 * @sync: sync object to add fence to
 * @fence: fence to sync to
 *
 */
int amdgpu_sync_fence(struct amdgpu_device *adev, struct amdgpu_sync *sync,
		      struct fence *f)
{
	struct amdgpu_sync_entry *e;
	struct amdgpu_fence *fence;

	if (!f)
		return 0;

	if (amdgpu_sync_same_dev(adev, f) &&
	    amdgpu_sync_test_owner(f, AMDGPU_FENCE_OWNER_VM))
		amdgpu_sync_keep_later(&sync->last_vm_update, f);

	fence = to_amdgpu_fence(f);
	if (!fence || fence->ring->adev != adev) {
		hash_for_each_possible(sync->fences, e, node, f->context) {
			if (unlikely(e->fence->context != f->context))
				continue;

			amdgpu_sync_keep_later(&e->fence, f);
			return 0;
		}

		e = kmalloc(sizeof(struct amdgpu_sync_entry), GFP_KERNEL);
		if (!e)
			return -ENOMEM;

		hash_add(sync->fences, &e->node, f->context);
		e->fence = fence_get(f);
		return 0;
	}

	amdgpu_sync_keep_later(&sync->sync_to[fence->ring->idx], f);

	return 0;
}

static void *amdgpu_sync_get_owner(struct fence *f)
{
	struct amdgpu_fence *a_fence = to_amdgpu_fence(f);
	struct amd_sched_fence *s_fence = to_amd_sched_fence(f);

	if (s_fence)
		return s_fence->owner;
	else if (a_fence)
		return a_fence->owner;
	return AMDGPU_FENCE_OWNER_UNDEFINED;
}

/**
 * amdgpu_sync_resv - sync to a reservation object
 *
 * @sync: sync object to add fences from reservation object to
 * @resv: reservation object with embedded fence
 * @shared: true if we should only sync to the exclusive fence
 *
 * Sync to the fence
 */
int amdgpu_sync_resv(struct amdgpu_device *adev,
		     struct amdgpu_sync *sync,
		     struct reservation_object *resv,
		     void *owner)
{
	struct reservation_object_list *flist;
	struct fence *f;
	void *fence_owner;
	unsigned i;
	int r = 0;

	if (resv == NULL)
		return -EINVAL;

	/* always sync to the exclusive fence */
	f = reservation_object_get_excl(resv);
	r = amdgpu_sync_fence(adev, sync, f);

	flist = reservation_object_get_list(resv);
	if (!flist || r)
		return r;

	for (i = 0; i < flist->shared_count; ++i) {
		f = rcu_dereference_protected(flist->shared[i],
					      reservation_object_held(resv));
		if (amdgpu_sync_same_dev(adev, f)) {
			/* VM updates are only interesting
			 * for other VM updates and moves.
			 */
			fence_owner = amdgpu_sync_get_owner(f);
			if ((owner != AMDGPU_FENCE_OWNER_UNDEFINED) &&
			    (fence_owner != AMDGPU_FENCE_OWNER_UNDEFINED) &&
			    ((owner == AMDGPU_FENCE_OWNER_VM) !=
			     (fence_owner == AMDGPU_FENCE_OWNER_VM)))
				continue;

			/* Ignore fence from the same owner as
			 * long as it isn't undefined.
			 */
			if (owner != AMDGPU_FENCE_OWNER_UNDEFINED &&
			    fence_owner == owner)
				continue;
		}

		r = amdgpu_sync_fence(adev, sync, f);
		if (r)
			break;
	}
	return r;
}

struct fence *amdgpu_sync_get_fence(struct amdgpu_sync *sync)
{
	struct amdgpu_sync_entry *e;
	struct hlist_node *tmp;
	struct fence *f;
	int i;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {

		f = e->fence;

		hash_del(&e->node);
		kfree(e);

		if (!fence_is_signaled(f))
			return f;

		fence_put(f);
	}
	return NULL;
}

int amdgpu_sync_wait(struct amdgpu_sync *sync)
{
	struct amdgpu_sync_entry *e;
	struct hlist_node *tmp;
	int i, r;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {
		r = fence_wait(e->fence, false);
		if (r)
			return r;

		hash_del(&e->node);
		fence_put(e->fence);
		kfree(e);
	}

	for (i = 0; i < AMDGPU_MAX_RINGS; ++i) {
		struct fence *fence = sync->sync_to[i];
		if (!fence)
			continue;

		r = fence_wait(fence, false);
		if (r)
			return r;
	}

	return 0;
}

/**
 * amdgpu_sync_free - free the sync object
 *
 * @adev: amdgpu_device pointer
 * @sync: sync object to use
 * @fence: fence to use for the free
 *
 * Free the sync object.
 */
void amdgpu_sync_free(struct amdgpu_device *adev,
		      struct amdgpu_sync *sync,
		      struct fence *fence)
{
	struct amdgpu_sync_entry *e;
	struct hlist_node *tmp;
	unsigned i;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {
		hash_del(&e->node);
		fence_put(e->fence);
		kfree(e);
	}

	for (i = 0; i < AMDGPU_MAX_RINGS; ++i)
		fence_put(sync->sync_to[i]);

	fence_put(sync->last_vm_update);
}
