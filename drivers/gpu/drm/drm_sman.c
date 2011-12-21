/**************************************************************************
 *
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck., ND., USA.
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
 *
 **************************************************************************/
/*
 * Simple memory manager interface that keeps track on allocate regions on a
 * per "owner" basis. All regions associated with an "owner" can be released
 * with a simple call. Typically if the "owner" exists. The owner is any
 * "unsigned long" identifier. Can typically be a pointer to a file private
 * struct or a context identifier.
 *
 * Authors:
 * Thomas Hellström <thomas-at-tungstengraphics-dot-com>
 */

#include <linux/export.h>
#include "drm_sman.h"

struct drm_owner_item {
	struct drm_hash_item owner_hash;
	struct list_head sman_list;
	struct list_head mem_blocks;
};

void drm_sman_takedown(struct drm_sman * sman)
{
	kfree(sman->mm);
}

EXPORT_SYMBOL(drm_sman_takedown);

int
drm_sman_init(struct drm_sman * sman, unsigned int num_managers,
	      unsigned int user_order, unsigned int owner_order)
{
	int ret = 0;

	sman->mm = kcalloc(num_managers, sizeof(*sman->mm), GFP_KERNEL);
	if (!sman->mm) {
		ret = -ENOMEM;
		return ret;
	}
	sman->num_managers = num_managers;

	return 0;
}

EXPORT_SYMBOL(drm_sman_init);

static void *drm_sman_mm_allocate(void *private, unsigned long size,
				  unsigned alignment)
{
	struct drm_mm *mm = (struct drm_mm *) private;
	struct drm_mm_node *tmp;

	tmp = drm_mm_search_free(mm, size, alignment, 1);
	if (!tmp) {
		return NULL;
	}
	tmp = drm_mm_get_block(tmp, size, alignment);
	return tmp;
}

static void drm_sman_mm_free(void *private, void *ref)
{
	struct drm_mm_node *node = (struct drm_mm_node *) ref;

	drm_mm_put_block(node);
}

static void drm_sman_mm_destroy(void *private)
{
	struct drm_mm *mm = (struct drm_mm *) private;
	drm_mm_takedown(mm);
	kfree(mm);
}

static unsigned long drm_sman_mm_offset(void *private, void *ref)
{
	struct drm_mm_node *node = (struct drm_mm_node *) ref;
	return node->start;
}

int
drm_sman_set_range(struct drm_sman * sman, unsigned int manager,
		   unsigned long start, unsigned long size)
{
	struct drm_sman_mm *sman_mm;
	struct drm_mm *mm;
	int ret;

	BUG_ON(manager >= sman->num_managers);

	sman_mm = &sman->mm[manager];
	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm) {
		return -ENOMEM;
	}
	sman_mm->private = mm;
	ret = drm_mm_init(mm, start, size);

	if (ret) {
		kfree(mm);
		return ret;
	}

	sman_mm->allocate = drm_sman_mm_allocate;
	sman_mm->free = drm_sman_mm_free;
	sman_mm->destroy = drm_sman_mm_destroy;
	sman_mm->offset = drm_sman_mm_offset;

	return 0;
}

EXPORT_SYMBOL(drm_sman_set_range);

int
drm_sman_set_manager(struct drm_sman * sman, unsigned int manager,
		     struct drm_sman_mm * allocator)
{
	BUG_ON(manager >= sman->num_managers);
	sman->mm[manager] = *allocator;

	return 0;
}
EXPORT_SYMBOL(drm_sman_set_manager);

struct drm_memblock_item *drm_sman_alloc(struct drm_sman *sman, unsigned int manager,
				    unsigned long size, unsigned alignment,
				    unsigned long owner)
{
	void *tmp;
	struct drm_sman_mm *sman_mm;
	struct drm_memblock_item *memblock;

	BUG_ON(manager >= sman->num_managers);

	sman_mm = &sman->mm[manager];
	tmp = sman_mm->allocate(sman_mm->private, size, alignment);

	if (!tmp) {
		return NULL;
	}

	memblock = kzalloc(sizeof(*memblock), GFP_KERNEL);

	if (!memblock)
		goto out;

	memblock->mm_info = tmp;
	memblock->mm = sman_mm;
	memblock->sman = sman;

	return memblock;

out:
	sman_mm->free(sman_mm->private, tmp);

	return NULL;
}

EXPORT_SYMBOL(drm_sman_alloc);

void drm_sman_free(struct drm_memblock_item *item)
{
	list_del(&item->owner_list);
	item->mm->free(item->mm->private, item->mm_info);
	kfree(item);
}
EXPORT_SYMBOL(drm_sman_free);

void drm_sman_cleanup(struct drm_sman *sman)
{
	unsigned int i;
	struct drm_sman_mm *sman_mm;

	if (sman->mm) {
		for (i = 0; i < sman->num_managers; ++i) {
			sman_mm = &sman->mm[i];
			if (sman_mm->private) {
				sman_mm->destroy(sman_mm->private);
				sman_mm->private = NULL;
			}
		}
	}
}

EXPORT_SYMBOL(drm_sman_cleanup);
