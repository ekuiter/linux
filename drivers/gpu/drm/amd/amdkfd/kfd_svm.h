/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2020-2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef KFD_SVM_H_
#define KFD_SVM_H_

#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/hmm.h>
#include "amdgpu.h"
#include "kfd_priv.h"

/**
 * struct svm_range - shared virtual memory range
 *
 * @svms:       list of svm ranges, structure defined in kfd_process
 * @start:      range start address in pages
 * @last:       range last address in pages
 * @it_node:    node [start, last] stored in interval tree, start, last are page
 *              aligned, page size is (last - start + 1)
 * @list:       link list node, used to scan all ranges of svms
 * @update_list:link list node used to add to update_list
 * @remove_list:link list node used to add to remove list
 * @insert_list:link list node used to add to insert list
 * @npages:     number of pages
 * @flags:      flags defined as KFD_IOCTL_SVM_FLAG_*
 * @perferred_loc: perferred location, 0 for CPU, or GPU id
 * @perfetch_loc: last prefetch location, 0 for CPU, or GPU id
 * @actual_loc: the actual location, 0 for CPU, or GPU id
 * @granularity:migration granularity, log2 num pages
 * @bitmap_access: index bitmap of GPUs which can access the range
 * @bitmap_aip: index bitmap of GPUs which can access the range in place
 *
 * Data structure for virtual memory range shared by CPU and GPUs, it can be
 * allocated from system memory ram or device vram, and migrate from ram to vram
 * or from vram to ram.
 */
struct svm_range {
	struct svm_range_list		*svms;
	unsigned long			start;
	unsigned long			last;
	struct interval_tree_node	it_node;
	struct list_head		list;
	struct list_head		update_list;
	struct list_head		remove_list;
	struct list_head		insert_list;
	uint64_t			npages;
	uint32_t			flags;
	uint32_t			preferred_loc;
	uint32_t			prefetch_loc;
	uint32_t			actual_loc;
	uint8_t				granularity;
	DECLARE_BITMAP(bitmap_access, MAX_GPU_INSTANCE);
	DECLARE_BITMAP(bitmap_aip, MAX_GPU_INSTANCE);
};

int svm_range_list_init(struct kfd_process *p);
void svm_range_list_fini(struct kfd_process *p);
int svm_ioctl(struct kfd_process *p, enum kfd_ioctl_svm_op op, uint64_t start,
	      uint64_t size, uint32_t nattrs,
	      struct kfd_ioctl_svm_attribute *attrs);

#endif /* KFD_SVM_H_ */
