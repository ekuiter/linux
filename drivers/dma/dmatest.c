/*
 * DMA Engine test module
 *
 * Copyright (C) 2007 Atmel Corporation
 * Copyright (C) 2013 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

static unsigned int test_buf_size = 16384;
module_param(test_buf_size, uint, S_IRUGO);
MODULE_PARM_DESC(test_buf_size, "Size of the memcpy test buffer");

static char test_channel[20];
module_param_string(channel, test_channel, sizeof(test_channel), S_IRUGO);
MODULE_PARM_DESC(channel, "Bus ID of the channel to test (default: any)");

static char test_device[20];
module_param_string(device, test_device, sizeof(test_device), S_IRUGO);
MODULE_PARM_DESC(device, "Bus ID of the DMA Engine to test (default: any)");

static unsigned int threads_per_chan = 1;
module_param(threads_per_chan, uint, S_IRUGO);
MODULE_PARM_DESC(threads_per_chan,
		"Number of threads to start per channel (default: 1)");

static unsigned int max_channels;
module_param(max_channels, uint, S_IRUGO);
MODULE_PARM_DESC(max_channels,
		"Maximum number of channels to use (default: all)");

static unsigned int iterations;
module_param(iterations, uint, S_IRUGO);
MODULE_PARM_DESC(iterations,
		"Iterations before stopping test (default: infinite)");

static unsigned int xor_sources = 3;
module_param(xor_sources, uint, S_IRUGO);
MODULE_PARM_DESC(xor_sources,
		"Number of xor source buffers (default: 3)");

static unsigned int pq_sources = 3;
module_param(pq_sources, uint, S_IRUGO);
MODULE_PARM_DESC(pq_sources,
		"Number of p+q source buffers (default: 3)");

static int timeout = 3000;
module_param(timeout, uint, S_IRUGO);
MODULE_PARM_DESC(timeout, "Transfer Timeout in msec (default: 3000), "
		 "Pass -1 for infinite timeout");

/*
 * Initialization patterns. All bytes in the source buffer has bit 7
 * set, all bytes in the destination buffer has bit 7 cleared.
 *
 * Bit 6 is set for all bytes which are to be copied by the DMA
 * engine. Bit 5 is set for all bytes which are to be overwritten by
 * the DMA engine.
 *
 * The remaining bits are the inverse of a counter which increments by
 * one for each byte address.
 */
#define PATTERN_SRC		0x80
#define PATTERN_DST		0x00
#define PATTERN_COPY		0x40
#define PATTERN_OVERWRITE	0x20
#define PATTERN_COUNT_MASK	0x1f

struct dmatest_info;

struct dmatest_thread {
	struct list_head	node;
	struct dmatest_info	*info;
	struct task_struct	*task;
	struct dma_chan		*chan;
	u8			**srcs;
	u8			**dsts;
	enum dma_transaction_type type;
};

struct dmatest_chan {
	struct list_head	node;
	struct dma_chan		*chan;
	struct list_head	threads;
};

/**
 * struct dmatest_params - test parameters.
 * @buf_size:		size of the memcpy test buffer
 * @channel:		bus ID of the channel to test
 * @device:		bus ID of the DMA Engine to test
 * @threads_per_chan:	number of threads to start per channel
 * @max_channels:	maximum number of channels to use
 * @iterations:		iterations before stopping test
 * @xor_sources:	number of xor source buffers
 * @pq_sources:		number of p+q source buffers
 * @timeout:		transfer timeout in msec, -1 for infinite timeout
 */
struct dmatest_params {
	unsigned int	buf_size;
	char		channel[20];
	char		device[20];
	unsigned int	threads_per_chan;
	unsigned int	max_channels;
	unsigned int	iterations;
	unsigned int	xor_sources;
	unsigned int	pq_sources;
	int		timeout;
};

/**
 * struct dmatest_info - test information.
 * @params:		test parameters
 * @lock:		access protection to the fields of this structure
 */
struct dmatest_info {
	/* Test parameters */
	struct dmatest_params	params;

	/* Internal state */
	struct list_head	channels;
	unsigned int		nr_channels;
	struct mutex		lock;

	/* debugfs related stuff */
	struct dentry		*root;
	struct dmatest_params	dbgfs_params;
};

static struct dmatest_info test_info;

static bool dmatest_match_channel(struct dmatest_params *params,
		struct dma_chan *chan)
{
	if (params->channel[0] == '\0')
		return true;
	return strcmp(dma_chan_name(chan), params->channel) == 0;
}

static bool dmatest_match_device(struct dmatest_params *params,
		struct dma_device *device)
{
	if (params->device[0] == '\0')
		return true;
	return strcmp(dev_name(device->dev), params->device) == 0;
}

static unsigned long dmatest_random(void)
{
	unsigned long buf;

	get_random_bytes(&buf, sizeof(buf));
	return buf;
}

static void dmatest_init_srcs(u8 **bufs, unsigned int start, unsigned int len,
		unsigned int buf_size)
{
	unsigned int i;
	u8 *buf;

	for (; (buf = *bufs); bufs++) {
		for (i = 0; i < start; i++)
			buf[i] = PATTERN_SRC | (~i & PATTERN_COUNT_MASK);
		for ( ; i < start + len; i++)
			buf[i] = PATTERN_SRC | PATTERN_COPY
				| (~i & PATTERN_COUNT_MASK);
		for ( ; i < buf_size; i++)
			buf[i] = PATTERN_SRC | (~i & PATTERN_COUNT_MASK);
		buf++;
	}
}

static void dmatest_init_dsts(u8 **bufs, unsigned int start, unsigned int len,
		unsigned int buf_size)
{
	unsigned int i;
	u8 *buf;

	for (; (buf = *bufs); bufs++) {
		for (i = 0; i < start; i++)
			buf[i] = PATTERN_DST | (~i & PATTERN_COUNT_MASK);
		for ( ; i < start + len; i++)
			buf[i] = PATTERN_DST | PATTERN_OVERWRITE
				| (~i & PATTERN_COUNT_MASK);
		for ( ; i < buf_size; i++)
			buf[i] = PATTERN_DST | (~i & PATTERN_COUNT_MASK);
	}
}

static void dmatest_mismatch(u8 actual, u8 pattern, unsigned int index,
		unsigned int counter, bool is_srcbuf)
{
	u8		diff = actual ^ pattern;
	u8		expected = pattern | (~counter & PATTERN_COUNT_MASK);
	const char	*thread_name = current->comm;

	if (is_srcbuf)
		pr_warning("%s: srcbuf[0x%x] overwritten!"
				" Expected %02x, got %02x\n",
				thread_name, index, expected, actual);
	else if ((pattern & PATTERN_COPY)
			&& (diff & (PATTERN_COPY | PATTERN_OVERWRITE)))
		pr_warning("%s: dstbuf[0x%x] not copied!"
				" Expected %02x, got %02x\n",
				thread_name, index, expected, actual);
	else if (diff & PATTERN_SRC)
		pr_warning("%s: dstbuf[0x%x] was copied!"
				" Expected %02x, got %02x\n",
				thread_name, index, expected, actual);
	else
		pr_warning("%s: dstbuf[0x%x] mismatch!"
				" Expected %02x, got %02x\n",
				thread_name, index, expected, actual);
}

static unsigned int dmatest_verify(u8 **bufs, unsigned int start,
		unsigned int end, unsigned int counter, u8 pattern,
		bool is_srcbuf)
{
	unsigned int i;
	unsigned int error_count = 0;
	u8 actual;
	u8 expected;
	u8 *buf;
	unsigned int counter_orig = counter;

	for (; (buf = *bufs); bufs++) {
		counter = counter_orig;
		for (i = start; i < end; i++) {
			actual = buf[i];
			expected = pattern | (~counter & PATTERN_COUNT_MASK);
			if (actual != expected) {
				if (error_count < 32)
					dmatest_mismatch(actual, pattern, i,
							 counter, is_srcbuf);
				error_count++;
			}
			counter++;
		}
	}

	if (error_count > 32)
		pr_warning("%s: %u errors suppressed\n",
			current->comm, error_count - 32);

	return error_count;
}

/* poor man's completion - we want to use wait_event_freezable() on it */
struct dmatest_done {
	bool			done;
	wait_queue_head_t	*wait;
};

static void dmatest_callback(void *arg)
{
	struct dmatest_done *done = arg;

	done->done = true;
	wake_up_all(done->wait);
}

static inline void unmap_src(struct device *dev, dma_addr_t *addr, size_t len,
			     unsigned int count)
{
	while (count--)
		dma_unmap_single(dev, addr[count], len, DMA_TO_DEVICE);
}

static inline void unmap_dst(struct device *dev, dma_addr_t *addr, size_t len,
			     unsigned int count)
{
	while (count--)
		dma_unmap_single(dev, addr[count], len, DMA_BIDIRECTIONAL);
}

static unsigned int min_odd(unsigned int x, unsigned int y)
{
	unsigned int val = min(x, y);

	return val % 2 ? val : val - 1;
}

/*
 * This function repeatedly tests DMA transfers of various lengths and
 * offsets for a given operation type until it is told to exit by
 * kthread_stop(). There may be multiple threads running this function
 * in parallel for a single channel, and there may be multiple channels
 * being tested in parallel.
 *
 * Before each test, the source and destination buffer is initialized
 * with a known pattern. This pattern is different depending on
 * whether it's in an area which is supposed to be copied or
 * overwritten, and different in the source and destination buffers.
 * So if the DMA engine doesn't copy exactly what we tell it to copy,
 * we'll notice.
 */
static int dmatest_func(void *data)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(done_wait);
	struct dmatest_thread	*thread = data;
	struct dmatest_done	done = { .wait = &done_wait };
	struct dmatest_info	*info;
	struct dmatest_params	*params;
	struct dma_chan		*chan;
	struct dma_device	*dev;
	const char		*thread_name;
	unsigned int		src_off, dst_off, len;
	unsigned int		error_count;
	unsigned int		failed_tests = 0;
	unsigned int		total_tests = 0;
	dma_cookie_t		cookie;
	enum dma_status		status;
	enum dma_ctrl_flags 	flags;
	u8			*pq_coefs = NULL;
	int			ret;
	int			src_cnt;
	int			dst_cnt;
	int			i;

	thread_name = current->comm;
	set_freezable();

	ret = -ENOMEM;

	smp_rmb();
	info = thread->info;
	params = &info->params;
	chan = thread->chan;
	dev = chan->device;
	if (thread->type == DMA_MEMCPY)
		src_cnt = dst_cnt = 1;
	else if (thread->type == DMA_XOR) {
		/* force odd to ensure dst = src */
		src_cnt = min_odd(params->xor_sources | 1, dev->max_xor);
		dst_cnt = 1;
	} else if (thread->type == DMA_PQ) {
		/* force odd to ensure dst = src */
		src_cnt = min_odd(params->pq_sources | 1, dma_maxpq(dev, 0));
		dst_cnt = 2;

		pq_coefs = kmalloc(params->pq_sources+1, GFP_KERNEL);
		if (!pq_coefs)
			goto err_thread_type;

		for (i = 0; i < src_cnt; i++)
			pq_coefs[i] = 1;
	} else
		goto err_thread_type;

	thread->srcs = kcalloc(src_cnt+1, sizeof(u8 *), GFP_KERNEL);
	if (!thread->srcs)
		goto err_srcs;
	for (i = 0; i < src_cnt; i++) {
		thread->srcs[i] = kmalloc(params->buf_size, GFP_KERNEL);
		if (!thread->srcs[i])
			goto err_srcbuf;
	}
	thread->srcs[i] = NULL;

	thread->dsts = kcalloc(dst_cnt+1, sizeof(u8 *), GFP_KERNEL);
	if (!thread->dsts)
		goto err_dsts;
	for (i = 0; i < dst_cnt; i++) {
		thread->dsts[i] = kmalloc(params->buf_size, GFP_KERNEL);
		if (!thread->dsts[i])
			goto err_dstbuf;
	}
	thread->dsts[i] = NULL;

	set_user_nice(current, 10);

	/*
	 * src buffers are freed by the DMAEngine code with dma_unmap_single()
	 * dst buffers are freed by ourselves below
	 */
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT
	      | DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SRC_UNMAP_SINGLE;

	while (!kthread_should_stop()
	       && !(params->iterations && total_tests >= params->iterations)) {
		struct dma_async_tx_descriptor *tx = NULL;
		dma_addr_t dma_srcs[src_cnt];
		dma_addr_t dma_dsts[dst_cnt];
		u8 align = 0;

		total_tests++;

		/* honor alignment restrictions */
		if (thread->type == DMA_MEMCPY)
			align = dev->copy_align;
		else if (thread->type == DMA_XOR)
			align = dev->xor_align;
		else if (thread->type == DMA_PQ)
			align = dev->pq_align;

		if (1 << align > params->buf_size) {
			pr_err("%u-byte buffer too small for %d-byte alignment\n",
			       params->buf_size, 1 << align);
			break;
		}

		len = dmatest_random() % params->buf_size + 1;
		len = (len >> align) << align;
		if (!len)
			len = 1 << align;
		src_off = dmatest_random() % (params->buf_size - len + 1);
		dst_off = dmatest_random() % (params->buf_size - len + 1);

		src_off = (src_off >> align) << align;
		dst_off = (dst_off >> align) << align;

		dmatest_init_srcs(thread->srcs, src_off, len, params->buf_size);
		dmatest_init_dsts(thread->dsts, dst_off, len, params->buf_size);

		for (i = 0; i < src_cnt; i++) {
			u8 *buf = thread->srcs[i] + src_off;

			dma_srcs[i] = dma_map_single(dev->dev, buf, len,
						     DMA_TO_DEVICE);
			ret = dma_mapping_error(dev->dev, dma_srcs[i]);
			if (ret) {
				unmap_src(dev->dev, dma_srcs, len, i);
				pr_warn("%s: #%u: mapping error %d with "
					"src_off=0x%x len=0x%x\n",
					thread_name, total_tests - 1, ret,
					src_off, len);
				failed_tests++;
				continue;
			}
		}
		/* map with DMA_BIDIRECTIONAL to force writeback/invalidate */
		for (i = 0; i < dst_cnt; i++) {
			dma_dsts[i] = dma_map_single(dev->dev, thread->dsts[i],
						     params->buf_size,
						     DMA_BIDIRECTIONAL);
			ret = dma_mapping_error(dev->dev, dma_dsts[i]);
			if (ret) {
				unmap_src(dev->dev, dma_srcs, len, src_cnt);
				unmap_dst(dev->dev, dma_dsts, params->buf_size,
					  i);
				pr_warn("%s: #%u: mapping error %d with "
					"dst_off=0x%x len=0x%x\n",
					thread_name, total_tests - 1, ret,
					dst_off, params->buf_size);
				failed_tests++;
				continue;
			}
		}

		if (thread->type == DMA_MEMCPY)
			tx = dev->device_prep_dma_memcpy(chan,
							 dma_dsts[0] + dst_off,
							 dma_srcs[0], len,
							 flags);
		else if (thread->type == DMA_XOR)
			tx = dev->device_prep_dma_xor(chan,
						      dma_dsts[0] + dst_off,
						      dma_srcs, src_cnt,
						      len, flags);
		else if (thread->type == DMA_PQ) {
			dma_addr_t dma_pq[dst_cnt];

			for (i = 0; i < dst_cnt; i++)
				dma_pq[i] = dma_dsts[i] + dst_off;
			tx = dev->device_prep_dma_pq(chan, dma_pq, dma_srcs,
						     src_cnt, pq_coefs,
						     len, flags);
		}

		if (!tx) {
			unmap_src(dev->dev, dma_srcs, len, src_cnt);
			unmap_dst(dev->dev, dma_dsts, params->buf_size,
				  dst_cnt);
			pr_warning("%s: #%u: prep error with src_off=0x%x "
					"dst_off=0x%x len=0x%x\n",
					thread_name, total_tests - 1,
					src_off, dst_off, len);
			msleep(100);
			failed_tests++;
			continue;
		}

		done.done = false;
		tx->callback = dmatest_callback;
		tx->callback_param = &done;
		cookie = tx->tx_submit(tx);

		if (dma_submit_error(cookie)) {
			pr_warning("%s: #%u: submit error %d with src_off=0x%x "
					"dst_off=0x%x len=0x%x\n",
					thread_name, total_tests - 1, cookie,
					src_off, dst_off, len);
			msleep(100);
			failed_tests++;
			continue;
		}
		dma_async_issue_pending(chan);

		wait_event_freezable_timeout(done_wait,
					     done.done || kthread_should_stop(),
					     msecs_to_jiffies(params->timeout));

		status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);

		if (!done.done) {
			/*
			 * We're leaving the timed out dma operation with
			 * dangling pointer to done_wait.  To make this
			 * correct, we'll need to allocate wait_done for
			 * each test iteration and perform "who's gonna
			 * free it this time?" dancing.  For now, just
			 * leave it dangling.
			 */
			pr_warning("%s: #%u: test timed out\n",
				   thread_name, total_tests - 1);
			failed_tests++;
			continue;
		} else if (status != DMA_SUCCESS) {
			pr_warning("%s: #%u: got completion callback,"
				   " but status is \'%s\'\n",
				   thread_name, total_tests - 1,
				   status == DMA_ERROR ? "error" : "in progress");
			failed_tests++;
			continue;
		}

		/* Unmap by myself (see DMA_COMPL_SKIP_DEST_UNMAP above) */
		unmap_dst(dev->dev, dma_dsts, params->buf_size, dst_cnt);

		error_count = 0;

		pr_debug("%s: verifying source buffer...\n", thread_name);
		error_count += dmatest_verify(thread->srcs, 0, src_off,
				0, PATTERN_SRC, true);
		error_count += dmatest_verify(thread->srcs, src_off,
				src_off + len, src_off,
				PATTERN_SRC | PATTERN_COPY, true);
		error_count += dmatest_verify(thread->srcs, src_off + len,
				params->buf_size, src_off + len,
				PATTERN_SRC, true);

		pr_debug("%s: verifying dest buffer...\n",
				thread->task->comm);
		error_count += dmatest_verify(thread->dsts, 0, dst_off,
				0, PATTERN_DST, false);
		error_count += dmatest_verify(thread->dsts, dst_off,
				dst_off + len, src_off,
				PATTERN_SRC | PATTERN_COPY, false);
		error_count += dmatest_verify(thread->dsts, dst_off + len,
				params->buf_size, dst_off + len,
				PATTERN_DST, false);

		if (error_count) {
			pr_warning("%s: #%u: %u errors with "
				"src_off=0x%x dst_off=0x%x len=0x%x\n",
				thread_name, total_tests - 1, error_count,
				src_off, dst_off, len);
			failed_tests++;
		} else {
			pr_debug("%s: #%u: No errors with "
				"src_off=0x%x dst_off=0x%x len=0x%x\n",
				thread_name, total_tests - 1,
				src_off, dst_off, len);
		}
	}

	ret = 0;
	for (i = 0; thread->dsts[i]; i++)
		kfree(thread->dsts[i]);
err_dstbuf:
	kfree(thread->dsts);
err_dsts:
	for (i = 0; thread->srcs[i]; i++)
		kfree(thread->srcs[i]);
err_srcbuf:
	kfree(thread->srcs);
err_srcs:
	kfree(pq_coefs);
err_thread_type:
	pr_notice("%s: terminating after %u tests, %u failures (status %d)\n",
			thread_name, total_tests, failed_tests, ret);

	/* terminate all transfers on specified channels */
	if (ret)
		dmaengine_terminate_all(chan);

	if (params->iterations > 0)
		while (!kthread_should_stop()) {
			DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wait_dmatest_exit);
			interruptible_sleep_on(&wait_dmatest_exit);
		}

	return ret;
}

static void dmatest_cleanup_channel(struct dmatest_chan *dtc)
{
	struct dmatest_thread	*thread;
	struct dmatest_thread	*_thread;
	int			ret;

	list_for_each_entry_safe(thread, _thread, &dtc->threads, node) {
		ret = kthread_stop(thread->task);
		pr_debug("dmatest: thread %s exited with status %d\n",
				thread->task->comm, ret);
		list_del(&thread->node);
		kfree(thread);
	}

	/* terminate all transfers on specified channels */
	dmaengine_terminate_all(dtc->chan);

	kfree(dtc);
}

static int dmatest_add_threads(struct dmatest_info *info,
		struct dmatest_chan *dtc, enum dma_transaction_type type)
{
	struct dmatest_params *params = &info->params;
	struct dmatest_thread *thread;
	struct dma_chan *chan = dtc->chan;
	char *op;
	unsigned int i;

	if (type == DMA_MEMCPY)
		op = "copy";
	else if (type == DMA_XOR)
		op = "xor";
	else if (type == DMA_PQ)
		op = "pq";
	else
		return -EINVAL;

	for (i = 0; i < params->threads_per_chan; i++) {
		thread = kzalloc(sizeof(struct dmatest_thread), GFP_KERNEL);
		if (!thread) {
			pr_warning("dmatest: No memory for %s-%s%u\n",
				   dma_chan_name(chan), op, i);

			break;
		}
		thread->info = info;
		thread->chan = dtc->chan;
		thread->type = type;
		smp_wmb();
		thread->task = kthread_run(dmatest_func, thread, "%s-%s%u",
				dma_chan_name(chan), op, i);
		if (IS_ERR(thread->task)) {
			pr_warning("dmatest: Failed to run thread %s-%s%u\n",
					dma_chan_name(chan), op, i);
			kfree(thread);
			break;
		}

		/* srcbuf and dstbuf are allocated by the thread itself */

		list_add_tail(&thread->node, &dtc->threads);
	}

	return i;
}

static int dmatest_add_channel(struct dmatest_info *info,
		struct dma_chan *chan)
{
	struct dmatest_chan	*dtc;
	struct dma_device	*dma_dev = chan->device;
	unsigned int		thread_count = 0;
	int cnt;

	dtc = kmalloc(sizeof(struct dmatest_chan), GFP_KERNEL);
	if (!dtc) {
		pr_warning("dmatest: No memory for %s\n", dma_chan_name(chan));
		return -ENOMEM;
	}

	dtc->chan = chan;
	INIT_LIST_HEAD(&dtc->threads);

	if (dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask)) {
		cnt = dmatest_add_threads(info, dtc, DMA_MEMCPY);
		thread_count += cnt > 0 ? cnt : 0;
	}
	if (dma_has_cap(DMA_XOR, dma_dev->cap_mask)) {
		cnt = dmatest_add_threads(info, dtc, DMA_XOR);
		thread_count += cnt > 0 ? cnt : 0;
	}
	if (dma_has_cap(DMA_PQ, dma_dev->cap_mask)) {
		cnt = dmatest_add_threads(info, dtc, DMA_PQ);
		thread_count += cnt > 0 ? cnt : 0;
	}

	pr_info("dmatest: Started %u threads using %s\n",
		thread_count, dma_chan_name(chan));

	list_add_tail(&dtc->node, &info->channels);
	info->nr_channels++;

	return 0;
}

static bool filter(struct dma_chan *chan, void *param)
{
	struct dmatest_params *params = param;

	if (!dmatest_match_channel(params, chan) ||
	    !dmatest_match_device(params, chan->device))
		return false;
	else
		return true;
}

static int __run_threaded_test(struct dmatest_info *info)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;
	struct dmatest_params *params = &info->params;
	int err = 0;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	for (;;) {
		chan = dma_request_channel(mask, filter, params);
		if (chan) {
			err = dmatest_add_channel(info, chan);
			if (err) {
				dma_release_channel(chan);
				break; /* add_channel failed, punt */
			}
		} else
			break; /* no more channels available */
		if (params->max_channels &&
		    info->nr_channels >= params->max_channels)
			break; /* we have all we need */
	}
	return err;
}

#ifndef MODULE
static int run_threaded_test(struct dmatest_info *info)
{
	int ret;

	mutex_lock(&info->lock);
	ret = __run_threaded_test(info);
	mutex_unlock(&info->lock);
	return ret;
}
#endif

static void __stop_threaded_test(struct dmatest_info *info)
{
	struct dmatest_chan *dtc, *_dtc;
	struct dma_chan *chan;

	list_for_each_entry_safe(dtc, _dtc, &info->channels, node) {
		list_del(&dtc->node);
		chan = dtc->chan;
		dmatest_cleanup_channel(dtc);
		pr_debug("dmatest: dropped channel %s\n", dma_chan_name(chan));
		dma_release_channel(chan);
	}

	info->nr_channels = 0;
}

static void stop_threaded_test(struct dmatest_info *info)
{
	mutex_lock(&info->lock);
	__stop_threaded_test(info);
	mutex_unlock(&info->lock);
}

static int __restart_threaded_test(struct dmatest_info *info, bool run)
{
	struct dmatest_params *params = &info->params;
	int ret;

	/* Stop any running test first */
	__stop_threaded_test(info);

	if (run == false)
		return 0;

	/* Copy test parameters */
	memcpy(params, &info->dbgfs_params, sizeof(*params));

	/* Run test with new parameters */
	ret = __run_threaded_test(info);
	if (ret) {
		__stop_threaded_test(info);
		pr_err("dmatest: Can't run test\n");
	}

	return ret;
}

static ssize_t dtf_write_string(void *to, size_t available, loff_t *ppos,
		const void __user *from, size_t count)
{
	char tmp[20];
	ssize_t len;

	len = simple_write_to_buffer(tmp, sizeof(tmp) - 1, ppos, from, count);
	if (len >= 0) {
		tmp[len] = '\0';
		strlcpy(to, strim(tmp), available);
	}

	return len;
}

static ssize_t dtf_read_channel(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	return simple_read_from_buffer(buf, count, ppos,
			info->dbgfs_params.channel,
			strlen(info->dbgfs_params.channel));
}

static ssize_t dtf_write_channel(struct file *file, const char __user *buf,
		size_t size, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	return dtf_write_string(info->dbgfs_params.channel,
				sizeof(info->dbgfs_params.channel),
				ppos, buf, size);
}

static const struct file_operations dtf_channel_fops = {
	.read	= dtf_read_channel,
	.write	= dtf_write_channel,
	.open	= simple_open,
	.llseek	= default_llseek,
};

static ssize_t dtf_read_device(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	return simple_read_from_buffer(buf, count, ppos,
			info->dbgfs_params.device,
			strlen(info->dbgfs_params.device));
}

static ssize_t dtf_write_device(struct file *file, const char __user *buf,
		size_t size, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	return dtf_write_string(info->dbgfs_params.device,
				sizeof(info->dbgfs_params.device),
				ppos, buf, size);
}

static const struct file_operations dtf_device_fops = {
	.read	= dtf_read_device,
	.write	= dtf_write_device,
	.open	= simple_open,
	.llseek	= default_llseek,
};

static ssize_t dtf_read_run(struct file *file, char __user *user_buf,
		size_t count, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	char buf[3];

	mutex_lock(&info->lock);
	if (info->nr_channels)
		buf[0] = 'Y';
	else
		buf[0] = 'N';
	mutex_unlock(&info->lock);
	buf[1] = '\n';
	buf[2] = 0x00;
	return simple_read_from_buffer(user_buf, count, ppos, buf, 2);
}

static ssize_t dtf_write_run(struct file *file, const char __user *user_buf,
		size_t count, loff_t *ppos)
{
	struct dmatest_info *info = file->private_data;
	char buf[16];
	bool bv;
	int ret = 0;

	if (copy_from_user(buf, user_buf, min(count, (sizeof(buf) - 1))))
		return -EFAULT;

	if (strtobool(buf, &bv) == 0) {
		mutex_lock(&info->lock);
		ret = __restart_threaded_test(info, bv);
		mutex_unlock(&info->lock);
	}

	return ret ? ret : count;
}

static const struct file_operations dtf_run_fops = {
	.read	= dtf_read_run,
	.write	= dtf_write_run,
	.open	= simple_open,
	.llseek	= default_llseek,
};

static int dmatest_register_dbgfs(struct dmatest_info *info)
{
	struct dentry *d;
	struct dmatest_params *params = &info->dbgfs_params;
	int ret = -ENOMEM;

	d = debugfs_create_dir("dmatest", NULL);
	if (IS_ERR(d))
		return PTR_ERR(d);
	if (!d)
		goto err_root;

	info->root = d;

	/* Copy initial values */
	memcpy(params, &info->params, sizeof(*params));

	/* Test parameters */

	d = debugfs_create_u32("test_buf_size", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->buf_size);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_file("channel", S_IRUGO | S_IWUSR, info->root,
				info, &dtf_channel_fops);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_file("device", S_IRUGO | S_IWUSR, info->root,
				info, &dtf_device_fops);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("threads_per_chan", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->threads_per_chan);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("max_channels", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->max_channels);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("iterations", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->iterations);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("xor_sources", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->xor_sources);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("pq_sources", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->pq_sources);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	d = debugfs_create_u32("timeout", S_IWUSR | S_IRUGO, info->root,
			       (u32 *)&params->timeout);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	/* Run or stop threaded test */
	d = debugfs_create_file("run", S_IWUSR | S_IRUGO, info->root,
				info, &dtf_run_fops);
	if (IS_ERR_OR_NULL(d))
		goto err_node;

	return 0;

err_node:
	debugfs_remove_recursive(info->root);
err_root:
	pr_err("dmatest: Failed to initialize debugfs\n");
	return ret;
}

static int __init dmatest_init(void)
{
	struct dmatest_info *info = &test_info;
	struct dmatest_params *params = &info->params;
	int ret;

	memset(info, 0, sizeof(*info));

	mutex_init(&info->lock);
	INIT_LIST_HEAD(&info->channels);

	/* Set default parameters */
	params->buf_size = test_buf_size;
	strlcpy(params->channel, test_channel, sizeof(params->channel));
	strlcpy(params->device, test_device, sizeof(params->device));
	params->threads_per_chan = threads_per_chan;
	params->max_channels = max_channels;
	params->iterations = iterations;
	params->xor_sources = xor_sources;
	params->pq_sources = pq_sources;
	params->timeout = timeout;

	ret = dmatest_register_dbgfs(info);
	if (ret)
		return ret;

#ifdef MODULE
	return 0;
#else
	return run_threaded_test(info);
#endif
}
/* when compiled-in wait for drivers to load first */
late_initcall(dmatest_init);

static void __exit dmatest_exit(void)
{
	struct dmatest_info *info = &test_info;

	debugfs_remove_recursive(info->root);
	stop_threaded_test(info);
}
module_exit(dmatest_exit);

MODULE_AUTHOR("Haavard Skinnemoen (Atmel)");
MODULE_LICENSE("GPL v2");
