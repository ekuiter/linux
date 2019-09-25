/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_MMAP_H
#define __LIBPERF_INTERNAL_MMAP_H

#include <linux/refcount.h>
#include <linux/types.h>

/**
 * struct perf_mmap - perf's ring buffer mmap details
 *
 * @refcnt - e.g. code using PERF_EVENT_IOC_SET_OUTPUT to share this
 */
struct perf_mmap {
	void		*base;
	int		 mask;
	int		 fd;
	int		 cpu;
	refcount_t	 refcnt;
	u64		 prev;
	u64		 start;
	u64		 end;
};

#endif /* __LIBPERF_INTERNAL_MMAP_H */
