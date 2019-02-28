// SPDX-License-Identifier: GPL-2.0
/*
 * Shared application/kernel submission and completion ring pairs, for
 * supporting fast/efficient IO.
 *
 * A note on the read/write ordering memory barriers that are matched between
 * the application and kernel side. When the application reads the CQ ring
 * tail, it must use an appropriate smp_rmb() to order with the smp_wmb()
 * the kernel uses after writing the tail. Failure to do so could cause a
 * delay in when the application notices that completion events available.
 * This isn't a fatal condition. Likewise, the application must use an
 * appropriate smp_wmb() both before writing the SQ tail, and after writing
 * the SQ tail. The first one orders the sqe writes with the tail write, and
 * the latter is paired with the smp_rmb() the kernel will issue before
 * reading the SQ tail on submission.
 *
 * Also see the examples in the liburing library:
 *
 *	git://git.kernel.dk/liburing
 *
 * io_uring also uses READ/WRITE_ONCE() for _any_ store or load that happens
 * from data shared between the kernel and application. This is done both
 * for ordering purposes, but also to ensure that once a value is loaded from
 * data that the application could potentially modify, it remains stable.
 *
 * Copyright (C) 2018-2019 Jens Axboe
 * Copyright (c) 2018-2019 Christoph Hellwig
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <linux/refcount.h>
#include <linux/uio.h>

#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_context.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/af_unix.h>
#include <linux/anon_inodes.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>

#include <uapi/linux/io_uring.h>

#include "internal.h"

#define IORING_MAX_ENTRIES	4096

struct io_uring {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

struct io_sq_ring {
	struct io_uring		r;
	u32			ring_mask;
	u32			ring_entries;
	u32			dropped;
	u32			flags;
	u32			array[];
};

struct io_cq_ring {
	struct io_uring		r;
	u32			ring_mask;
	u32			ring_entries;
	u32			overflow;
	struct io_uring_cqe	cqes[];
};

struct io_ring_ctx {
	struct {
		struct percpu_ref	refs;
	} ____cacheline_aligned_in_smp;

	struct {
		unsigned int		flags;
		bool			compat;
		bool			account_mem;

		/* SQ ring */
		struct io_sq_ring	*sq_ring;
		unsigned		cached_sq_head;
		unsigned		sq_entries;
		unsigned		sq_mask;
		struct io_uring_sqe	*sq_sqes;
	} ____cacheline_aligned_in_smp;

	/* IO offload */
	struct workqueue_struct	*sqo_wq;
	struct mm_struct	*sqo_mm;

	struct {
		/* CQ ring */
		struct io_cq_ring	*cq_ring;
		unsigned		cached_cq_tail;
		unsigned		cq_entries;
		unsigned		cq_mask;
		struct wait_queue_head	cq_wait;
		struct fasync_struct	*cq_fasync;
	} ____cacheline_aligned_in_smp;

	struct user_struct	*user;

	struct completion	ctx_done;

	struct {
		struct mutex		uring_lock;
		wait_queue_head_t	wait;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t		completion_lock;
		bool			poll_multi_file;
		/*
		 * ->poll_list is protected by the ctx->uring_lock for
		 * io_uring instances that don't use IORING_SETUP_SQPOLL.
		 * For SQPOLL, only the single threaded io_sq_thread() will
		 * manipulate the list, hence no extra locking is needed there.
		 */
		struct list_head	poll_list;
	} ____cacheline_aligned_in_smp;

#if defined(CONFIG_UNIX)
	struct socket		*ring_sock;
#endif
};

struct sqe_submit {
	const struct io_uring_sqe	*sqe;
	unsigned short			index;
	bool				has_user;
	bool				needs_lock;
};

struct io_kiocb {
	struct kiocb		rw;

	struct sqe_submit	submit;

	struct io_ring_ctx	*ctx;
	struct list_head	list;
	unsigned int		flags;
#define REQ_F_FORCE_NONBLOCK	1	/* inline submission attempt */
#define REQ_F_IOPOLL_COMPLETED	2	/* polled IO has completed */
	u64			user_data;
	u64			error;

	struct work_struct	work;
};

#define IO_PLUG_THRESHOLD		2
#define IO_IOPOLL_BATCH			8

struct io_submit_state {
	struct blk_plug		plug;

	/*
	 * io_kiocb alloc cache
	 */
	void			*reqs[IO_IOPOLL_BATCH];
	unsigned		int free_reqs;
	unsigned		int cur_req;

	/*
	 * File reference cache
	 */
	struct file		*file;
	unsigned int		fd;
	unsigned int		has_refs;
	unsigned int		used_refs;
	unsigned int		ios_left;
};

static struct kmem_cache *req_cachep;

static const struct file_operations io_uring_fops;

struct sock *io_uring_get_socket(struct file *file)
{
#if defined(CONFIG_UNIX)
	if (file->f_op == &io_uring_fops) {
		struct io_ring_ctx *ctx = file->private_data;

		return ctx->ring_sock->sk;
	}
#endif
	return NULL;
}
EXPORT_SYMBOL(io_uring_get_socket);

static void io_ring_ctx_ref_free(struct percpu_ref *ref)
{
	struct io_ring_ctx *ctx = container_of(ref, struct io_ring_ctx, refs);

	complete(&ctx->ctx_done);
}

static struct io_ring_ctx *io_ring_ctx_alloc(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	if (percpu_ref_init(&ctx->refs, io_ring_ctx_ref_free, 0, GFP_KERNEL)) {
		kfree(ctx);
		return NULL;
	}

	ctx->flags = p->flags;
	init_waitqueue_head(&ctx->cq_wait);
	init_completion(&ctx->ctx_done);
	mutex_init(&ctx->uring_lock);
	init_waitqueue_head(&ctx->wait);
	spin_lock_init(&ctx->completion_lock);
	INIT_LIST_HEAD(&ctx->poll_list);
	return ctx;
}

static void io_commit_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring;

	if (ctx->cached_cq_tail != READ_ONCE(ring->r.tail)) {
		/* order cqe stores with ring update */
		smp_store_release(&ring->r.tail, ctx->cached_cq_tail);

		/*
		 * Write sider barrier of tail update, app has read side. See
		 * comment at the top of this file.
		 */
		smp_wmb();

		if (wq_has_sleeper(&ctx->cq_wait)) {
			wake_up_interruptible(&ctx->cq_wait);
			kill_fasync(&ctx->cq_fasync, SIGIO, POLL_IN);
		}
	}
}

static struct io_uring_cqe *io_get_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring;
	unsigned tail;

	tail = ctx->cached_cq_tail;
	/* See comment at the top of the file */
	smp_rmb();
	if (tail + 1 == READ_ONCE(ring->r.head))
		return NULL;

	ctx->cached_cq_tail++;
	return &ring->cqes[tail & ctx->cq_mask];
}

static void io_cqring_fill_event(struct io_ring_ctx *ctx, u64 ki_user_data,
				 long res, unsigned ev_flags)
{
	struct io_uring_cqe *cqe;

	/*
	 * If we can't get a cq entry, userspace overflowed the
	 * submission (by quite a lot). Increment the overflow count in
	 * the ring.
	 */
	cqe = io_get_cqring(ctx);
	if (cqe) {
		WRITE_ONCE(cqe->user_data, ki_user_data);
		WRITE_ONCE(cqe->res, res);
		WRITE_ONCE(cqe->flags, ev_flags);
	} else {
		unsigned overflow = READ_ONCE(ctx->cq_ring->overflow);

		WRITE_ONCE(ctx->cq_ring->overflow, overflow + 1);
	}
}

static void io_cqring_add_event(struct io_ring_ctx *ctx, u64 ki_user_data,
				long res, unsigned ev_flags)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->completion_lock, flags);
	io_cqring_fill_event(ctx, ki_user_data, res, ev_flags);
	io_commit_cqring(ctx);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
}

static void io_ring_drop_ctx_refs(struct io_ring_ctx *ctx, unsigned refs)
{
	percpu_ref_put_many(&ctx->refs, refs);

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
}

static struct io_kiocb *io_get_req(struct io_ring_ctx *ctx,
				   struct io_submit_state *state)
{
	struct io_kiocb *req;

	if (!percpu_ref_tryget(&ctx->refs))
		return NULL;

	if (!state) {
		req = kmem_cache_alloc(req_cachep, __GFP_NOWARN);
		if (unlikely(!req))
			goto out;
	} else if (!state->free_reqs) {
		size_t sz;
		int ret;

		sz = min_t(size_t, state->ios_left, ARRAY_SIZE(state->reqs));
		ret = kmem_cache_alloc_bulk(req_cachep, __GFP_NOWARN, sz,
						state->reqs);
		if (unlikely(ret <= 0))
			goto out;
		state->free_reqs = ret - 1;
		state->cur_req = 1;
		req = state->reqs[0];
	} else {
		req = state->reqs[state->cur_req];
		state->free_reqs--;
		state->cur_req++;
	}

	req->ctx = ctx;
	req->flags = 0;
	return req;
out:
	io_ring_drop_ctx_refs(ctx, 1);
	return NULL;
}

static void io_free_req_many(struct io_ring_ctx *ctx, void **reqs, int *nr)
{
	if (*nr) {
		kmem_cache_free_bulk(req_cachep, *nr, reqs);
		io_ring_drop_ctx_refs(ctx, *nr);
		*nr = 0;
	}
}

static void io_free_req(struct io_kiocb *req)
{
	io_ring_drop_ctx_refs(req->ctx, 1);
	kmem_cache_free(req_cachep, req);
}

/*
 * Find and free completed poll iocbs
 */
static void io_iopoll_complete(struct io_ring_ctx *ctx, unsigned int *nr_events,
			       struct list_head *done)
{
	void *reqs[IO_IOPOLL_BATCH];
	int file_count, to_free;
	struct file *file = NULL;
	struct io_kiocb *req;

	file_count = to_free = 0;
	while (!list_empty(done)) {
		req = list_first_entry(done, struct io_kiocb, list);
		list_del(&req->list);

		io_cqring_fill_event(ctx, req->user_data, req->error, 0);

		reqs[to_free++] = req;
		(*nr_events)++;

		/*
		 * Batched puts of the same file, to avoid dirtying the
		 * file usage count multiple times, if avoidable.
		 */
		if (!file) {
			file = req->rw.ki_filp;
			file_count = 1;
		} else if (file == req->rw.ki_filp) {
			file_count++;
		} else {
			fput_many(file, file_count);
			file = req->rw.ki_filp;
			file_count = 1;
		}

		if (to_free == ARRAY_SIZE(reqs))
			io_free_req_many(ctx, reqs, &to_free);
	}
	io_commit_cqring(ctx);

	if (file)
		fput_many(file, file_count);
	io_free_req_many(ctx, reqs, &to_free);
}

static int io_do_iopoll(struct io_ring_ctx *ctx, unsigned int *nr_events,
			long min)
{
	struct io_kiocb *req, *tmp;
	LIST_HEAD(done);
	bool spin;
	int ret;

	/*
	 * Only spin for completions if we don't have multiple devices hanging
	 * off our complete list, and we're under the requested amount.
	 */
	spin = !ctx->poll_multi_file && *nr_events < min;

	ret = 0;
	list_for_each_entry_safe(req, tmp, &ctx->poll_list, list) {
		struct kiocb *kiocb = &req->rw;

		/*
		 * Move completed entries to our local list. If we find a
		 * request that requires polling, break out and complete
		 * the done list first, if we have entries there.
		 */
		if (req->flags & REQ_F_IOPOLL_COMPLETED) {
			list_move_tail(&req->list, &done);
			continue;
		}
		if (!list_empty(&done))
			break;

		ret = kiocb->ki_filp->f_op->iopoll(kiocb, spin);
		if (ret < 0)
			break;

		if (ret && spin)
			spin = false;
		ret = 0;
	}

	if (!list_empty(&done))
		io_iopoll_complete(ctx, nr_events, &done);

	return ret;
}

/*
 * Poll for a mininum of 'min' events. Note that if min == 0 we consider that a
 * non-spinning poll check - we'll still enter the driver poll loop, but only
 * as a non-spinning completion check.
 */
static int io_iopoll_getevents(struct io_ring_ctx *ctx, unsigned int *nr_events,
				long min)
{
	while (!list_empty(&ctx->poll_list)) {
		int ret;

		ret = io_do_iopoll(ctx, nr_events, min);
		if (ret < 0)
			return ret;
		if (!min || *nr_events >= min)
			return 0;
	}

	return 1;
}

/*
 * We can't just wait for polled events to come to us, we have to actively
 * find and complete them.
 */
static void io_iopoll_reap_events(struct io_ring_ctx *ctx)
{
	if (!(ctx->flags & IORING_SETUP_IOPOLL))
		return;

	mutex_lock(&ctx->uring_lock);
	while (!list_empty(&ctx->poll_list)) {
		unsigned int nr_events = 0;

		io_iopoll_getevents(ctx, &nr_events, 1);
	}
	mutex_unlock(&ctx->uring_lock);
}

static int io_iopoll_check(struct io_ring_ctx *ctx, unsigned *nr_events,
			   long min)
{
	int ret = 0;

	do {
		int tmin = 0;

		if (*nr_events < min)
			tmin = min - *nr_events;

		ret = io_iopoll_getevents(ctx, nr_events, tmin);
		if (ret <= 0)
			break;
		ret = 0;
	} while (min && !*nr_events && !need_resched());

	return ret;
}

static void kiocb_end_write(struct kiocb *kiocb)
{
	if (kiocb->ki_flags & IOCB_WRITE) {
		struct inode *inode = file_inode(kiocb->ki_filp);

		/*
		 * Tell lockdep we inherited freeze protection from submission
		 * thread.
		 */
		if (S_ISREG(inode->i_mode))
			__sb_writers_acquired(inode->i_sb, SB_FREEZE_WRITE);
		file_end_write(kiocb->ki_filp);
	}
}

static void io_complete_rw(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	fput(kiocb->ki_filp);
	io_cqring_add_event(req->ctx, req->user_data, res, 0);
	io_free_req(req);
}

static void io_complete_rw_iopoll(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	req->error = res;
	if (res != -EAGAIN)
		req->flags |= REQ_F_IOPOLL_COMPLETED;
}

/*
 * After the iocb has been issued, it's safe to be found on the poll list.
 * Adding the kiocb to the list AFTER submission ensures that we don't
 * find it from a io_iopoll_getevents() thread before the issuer is done
 * accessing the kiocb cookie.
 */
static void io_iopoll_req_issued(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;

	/*
	 * Track whether we have multiple files in our lists. This will impact
	 * how we do polling eventually, not spinning if we're on potentially
	 * different devices.
	 */
	if (list_empty(&ctx->poll_list)) {
		ctx->poll_multi_file = false;
	} else if (!ctx->poll_multi_file) {
		struct io_kiocb *list_req;

		list_req = list_first_entry(&ctx->poll_list, struct io_kiocb,
						list);
		if (list_req->rw.ki_filp != req->rw.ki_filp)
			ctx->poll_multi_file = true;
	}

	/*
	 * For fast devices, IO may have already completed. If it has, add
	 * it to the front so we find it first.
	 */
	if (req->flags & REQ_F_IOPOLL_COMPLETED)
		list_add(&req->list, &ctx->poll_list);
	else
		list_add_tail(&req->list, &ctx->poll_list);
}

static void io_file_put(struct io_submit_state *state, struct file *file)
{
	if (!state) {
		fput(file);
	} else if (state->file) {
		int diff = state->has_refs - state->used_refs;

		if (diff)
			fput_many(state->file, diff);
		state->file = NULL;
	}
}

/*
 * Get as many references to a file as we have IOs left in this submission,
 * assuming most submissions are for one file, or at least that each file
 * has more than one submission.
 */
static struct file *io_file_get(struct io_submit_state *state, int fd)
{
	if (!state)
		return fget(fd);

	if (state->file) {
		if (state->fd == fd) {
			state->used_refs++;
			state->ios_left--;
			return state->file;
		}
		io_file_put(state, NULL);
	}
	state->file = fget_many(fd, state->ios_left);
	if (!state->file)
		return NULL;

	state->fd = fd;
	state->has_refs = state->ios_left;
	state->used_refs = 1;
	state->ios_left--;
	return state->file;
}

/*
 * If we tracked the file through the SCM inflight mechanism, we could support
 * any file. For now, just ensure that anything potentially problematic is done
 * inline.
 */
static bool io_file_supports_async(struct file *file)
{
	umode_t mode = file_inode(file)->i_mode;

	if (S_ISBLK(mode) || S_ISCHR(mode))
		return true;
	if (S_ISREG(mode) && file->f_op != &io_uring_fops)
		return true;

	return false;
}

static int io_prep_rw(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		      bool force_nonblock, struct io_submit_state *state)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct kiocb *kiocb = &req->rw;
	unsigned ioprio;
	int fd, ret;

	/* For -EAGAIN retry, everything is already prepped */
	if (kiocb->ki_filp)
		return 0;

	fd = READ_ONCE(sqe->fd);
	kiocb->ki_filp = io_file_get(state, fd);
	if (unlikely(!kiocb->ki_filp))
		return -EBADF;
	if (force_nonblock && !io_file_supports_async(kiocb->ki_filp))
		force_nonblock = false;
	kiocb->ki_pos = READ_ONCE(sqe->off);
	kiocb->ki_flags = iocb_flags(kiocb->ki_filp);
	kiocb->ki_hint = ki_hint_validate(file_write_hint(kiocb->ki_filp));

	ioprio = READ_ONCE(sqe->ioprio);
	if (ioprio) {
		ret = ioprio_check_cap(ioprio);
		if (ret)
			goto out_fput;

		kiocb->ki_ioprio = ioprio;
	} else
		kiocb->ki_ioprio = get_current_ioprio();

	ret = kiocb_set_rw_flags(kiocb, READ_ONCE(sqe->rw_flags));
	if (unlikely(ret))
		goto out_fput;
	if (force_nonblock) {
		kiocb->ki_flags |= IOCB_NOWAIT;
		req->flags |= REQ_F_FORCE_NONBLOCK;
	}
	if (ctx->flags & IORING_SETUP_IOPOLL) {
		ret = -EOPNOTSUPP;
		if (!(kiocb->ki_flags & IOCB_DIRECT) ||
		    !kiocb->ki_filp->f_op->iopoll)
			goto out_fput;

		req->error = 0;
		kiocb->ki_flags |= IOCB_HIPRI;
		kiocb->ki_complete = io_complete_rw_iopoll;
	} else {
		if (kiocb->ki_flags & IOCB_HIPRI) {
			ret = -EINVAL;
			goto out_fput;
		}
		kiocb->ki_complete = io_complete_rw;
	}
	return 0;
out_fput:
	/* in case of error, we didn't use this file reference. drop it. */
	if (state)
		state->used_refs--;
	io_file_put(state, kiocb->ki_filp);
	return ret;
}

static inline void io_rw_done(struct kiocb *kiocb, ssize_t ret)
{
	switch (ret) {
	case -EIOCBQUEUED:
		break;
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		/*
		 * We can't just restart the syscall, since previously
		 * submitted sqes may already be in progress. Just fail this
		 * IO with EINTR.
		 */
		ret = -EINTR;
		/* fall through */
	default:
		kiocb->ki_complete(kiocb, ret, 0);
	}
}

static int io_import_iovec(struct io_ring_ctx *ctx, int rw,
			   const struct sqe_submit *s, struct iovec **iovec,
			   struct iov_iter *iter)
{
	const struct io_uring_sqe *sqe = s->sqe;
	void __user *buf = u64_to_user_ptr(READ_ONCE(sqe->addr));
	size_t sqe_len = READ_ONCE(sqe->len);

	if (!s->has_user)
		return -EFAULT;

#ifdef CONFIG_COMPAT
	if (ctx->compat)
		return compat_import_iovec(rw, buf, sqe_len, UIO_FASTIOV,
						iovec, iter);
#endif

	return import_iovec(rw, buf, sqe_len, UIO_FASTIOV, iovec, iter);
}

static ssize_t io_read(struct io_kiocb *req, const struct sqe_submit *s,
		       bool force_nonblock, struct io_submit_state *state)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(req, s->sqe, force_nonblock, state);
	if (ret)
		return ret;
	file = kiocb->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_READ)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->read_iter))
		goto out_fput;

	ret = io_import_iovec(req->ctx, READ, s, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(READ, file, &kiocb->ki_pos, iov_iter_count(&iter));
	if (!ret) {
		ssize_t ret2;

		/* Catch -EAGAIN return for forced non-blocking submission */
		ret2 = call_read_iter(file, kiocb, &iter);
		if (!force_nonblock || ret2 != -EAGAIN)
			io_rw_done(kiocb, ret2);
		else
			ret = -EAGAIN;
	}
	kfree(iovec);
out_fput:
	/* Hold on to the file for -EAGAIN */
	if (unlikely(ret && ret != -EAGAIN))
		fput(file);
	return ret;
}

static ssize_t io_write(struct io_kiocb *req, const struct sqe_submit *s,
			bool force_nonblock, struct io_submit_state *state)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(req, s->sqe, force_nonblock, state);
	if (ret)
		return ret;
	/* Hold on to the file for -EAGAIN */
	if (force_nonblock && !(kiocb->ki_flags & IOCB_DIRECT))
		return -EAGAIN;

	ret = -EBADF;
	file = kiocb->ki_filp;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->write_iter))
		goto out_fput;

	ret = io_import_iovec(req->ctx, WRITE, s, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(WRITE, file, &kiocb->ki_pos,
				iov_iter_count(&iter));
	if (!ret) {
		/*
		 * Open-code file_start_write here to grab freeze protection,
		 * which will be released by another thread in
		 * io_complete_rw().  Fool lockdep by telling it the lock got
		 * released so that it doesn't complain about the held lock when
		 * we return to userspace.
		 */
		if (S_ISREG(file_inode(file)->i_mode)) {
			__sb_start_write(file_inode(file)->i_sb,
						SB_FREEZE_WRITE, true);
			__sb_writers_release(file_inode(file)->i_sb,
						SB_FREEZE_WRITE);
		}
		kiocb->ki_flags |= IOCB_WRITE;
		io_rw_done(kiocb, call_write_iter(file, kiocb, &iter));
	}
	kfree(iovec);
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

/*
 * IORING_OP_NOP just posts a completion event, nothing else.
 */
static int io_nop(struct io_kiocb *req, u64 user_data)
{
	struct io_ring_ctx *ctx = req->ctx;
	long err = 0;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;

	/*
	 * Twilight zone - it's possible that someone issued an opcode that
	 * has a file attached, then got -EAGAIN on submission, and changed
	 * the sqe before we retried it from async context. Avoid dropping
	 * a file reference for this malicious case, and flag the error.
	 */
	if (req->rw.ki_filp) {
		err = -EBADF;
		fput(req->rw.ki_filp);
	}
	io_cqring_add_event(ctx, user_data, err, 0);
	io_free_req(req);
	return 0;
}

static int io_prep_fsync(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	int fd;

	/* Prep already done */
	if (req->rw.ki_filp)
		return 0;

	if (unlikely(req->ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (unlikely(sqe->addr || sqe->ioprio))
		return -EINVAL;

	fd = READ_ONCE(sqe->fd);
	req->rw.ki_filp = fget(fd);
	if (unlikely(!req->rw.ki_filp))
		return -EBADF;

	return 0;
}

static int io_fsync(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		    bool force_nonblock)
{
	loff_t sqe_off = READ_ONCE(sqe->off);
	loff_t sqe_len = READ_ONCE(sqe->len);
	loff_t end = sqe_off + sqe_len;
	unsigned fsync_flags;
	int ret;

	fsync_flags = READ_ONCE(sqe->fsync_flags);
	if (unlikely(fsync_flags & ~IORING_FSYNC_DATASYNC))
		return -EINVAL;

	ret = io_prep_fsync(req, sqe);
	if (ret)
		return ret;

	/* fsync always requires a blocking context */
	if (force_nonblock)
		return -EAGAIN;

	ret = vfs_fsync_range(req->rw.ki_filp, sqe_off,
				end > 0 ? end : LLONG_MAX,
				fsync_flags & IORING_FSYNC_DATASYNC);

	fput(req->rw.ki_filp);
	io_cqring_add_event(req->ctx, sqe->user_data, ret, 0);
	io_free_req(req);
	return 0;
}

static int __io_submit_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			   const struct sqe_submit *s, bool force_nonblock,
			   struct io_submit_state *state)
{
	ssize_t ret;
	int opcode;

	if (unlikely(s->index >= ctx->sq_entries))
		return -EINVAL;
	req->user_data = READ_ONCE(s->sqe->user_data);

	opcode = READ_ONCE(s->sqe->opcode);
	switch (opcode) {
	case IORING_OP_NOP:
		ret = io_nop(req, req->user_data);
		break;
	case IORING_OP_READV:
		ret = io_read(req, s, force_nonblock, state);
		break;
	case IORING_OP_WRITEV:
		ret = io_write(req, s, force_nonblock, state);
		break;
	case IORING_OP_FSYNC:
		ret = io_fsync(req, s->sqe, force_nonblock);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (req->error == -EAGAIN)
			return -EAGAIN;

		/* workqueue context doesn't hold uring_lock, grab it now */
		if (s->needs_lock)
			mutex_lock(&ctx->uring_lock);
		io_iopoll_req_issued(req);
		if (s->needs_lock)
			mutex_unlock(&ctx->uring_lock);
	}

	return 0;
}

static void io_sq_wq_submit_work(struct work_struct *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	struct sqe_submit *s = &req->submit;
	const struct io_uring_sqe *sqe = s->sqe;
	struct io_ring_ctx *ctx = req->ctx;
	mm_segment_t old_fs = get_fs();
	int ret;

	 /* Ensure we clear previously set forced non-block flag */
	req->flags &= ~REQ_F_FORCE_NONBLOCK;
	req->rw.ki_flags &= ~IOCB_NOWAIT;

	if (!mmget_not_zero(ctx->sqo_mm)) {
		ret = -EFAULT;
		goto err;
	}

	use_mm(ctx->sqo_mm);
	set_fs(USER_DS);
	s->has_user = true;
	s->needs_lock = true;

	do {
		ret = __io_submit_sqe(ctx, req, s, false, NULL);
		/*
		 * We can get EAGAIN for polled IO even though we're forcing
		 * a sync submission from here, since we can't wait for
		 * request slots on the block side.
		 */
		if (ret != -EAGAIN)
			break;
		cond_resched();
	} while (1);

	set_fs(old_fs);
	unuse_mm(ctx->sqo_mm);
	mmput(ctx->sqo_mm);
err:
	if (ret) {
		io_cqring_add_event(ctx, sqe->user_data, ret, 0);
		io_free_req(req);
	}

	/* async context always use a copy of the sqe */
	kfree(sqe);
}

static int io_submit_sqe(struct io_ring_ctx *ctx, struct sqe_submit *s,
			 struct io_submit_state *state)
{
	struct io_kiocb *req;
	ssize_t ret;

	/* enforce forwards compatibility on users */
	if (unlikely(s->sqe->flags))
		return -EINVAL;

	req = io_get_req(ctx, state);
	if (unlikely(!req))
		return -EAGAIN;

	req->rw.ki_filp = NULL;

	ret = __io_submit_sqe(ctx, req, s, true, state);
	if (ret == -EAGAIN) {
		struct io_uring_sqe *sqe_copy;

		sqe_copy = kmalloc(sizeof(*sqe_copy), GFP_KERNEL);
		if (sqe_copy) {
			memcpy(sqe_copy, s->sqe, sizeof(*sqe_copy));
			s->sqe = sqe_copy;

			memcpy(&req->submit, s, sizeof(*s));
			INIT_WORK(&req->work, io_sq_wq_submit_work);
			queue_work(ctx->sqo_wq, &req->work);
			ret = 0;
		}
	}
	if (ret)
		io_free_req(req);

	return ret;
}

/*
 * Batched submission is done, ensure local IO is flushed out.
 */
static void io_submit_state_end(struct io_submit_state *state)
{
	blk_finish_plug(&state->plug);
	io_file_put(state, NULL);
	if (state->free_reqs)
		kmem_cache_free_bulk(req_cachep, state->free_reqs,
					&state->reqs[state->cur_req]);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  struct io_ring_ctx *ctx, unsigned max_ios)
{
	blk_start_plug(&state->plug);
	state->free_reqs = 0;
	state->file = NULL;
	state->ios_left = max_ios;
}

static void io_commit_sqring(struct io_ring_ctx *ctx)
{
	struct io_sq_ring *ring = ctx->sq_ring;

	if (ctx->cached_sq_head != READ_ONCE(ring->r.head)) {
		/*
		 * Ensure any loads from the SQEs are done at this point,
		 * since once we write the new head, the application could
		 * write new data to them.
		 */
		smp_store_release(&ring->r.head, ctx->cached_sq_head);

		/*
		 * write side barrier of head update, app has read side. See
		 * comment at the top of this file
		 */
		smp_wmb();
	}
}

/*
 * Undo last io_get_sqring()
 */
static void io_drop_sqring(struct io_ring_ctx *ctx)
{
	ctx->cached_sq_head--;
}

/*
 * Fetch an sqe, if one is available. Note that s->sqe will point to memory
 * that is mapped by userspace. This means that care needs to be taken to
 * ensure that reads are stable, as we cannot rely on userspace always
 * being a good citizen. If members of the sqe are validated and then later
 * used, it's important that those reads are done through READ_ONCE() to
 * prevent a re-load down the line.
 */
static bool io_get_sqring(struct io_ring_ctx *ctx, struct sqe_submit *s)
{
	struct io_sq_ring *ring = ctx->sq_ring;
	unsigned head;

	/*
	 * The cached sq head (or cq tail) serves two purposes:
	 *
	 * 1) allows us to batch the cost of updating the user visible
	 *    head updates.
	 * 2) allows the kernel side to track the head on its own, even
	 *    though the application is the one updating it.
	 */
	head = ctx->cached_sq_head;
	/* See comment at the top of this file */
	smp_rmb();
	if (head == READ_ONCE(ring->r.tail))
		return false;

	head = READ_ONCE(ring->array[head & ctx->sq_mask]);
	if (head < ctx->sq_entries) {
		s->index = head;
		s->sqe = &ctx->sq_sqes[head];
		ctx->cached_sq_head++;
		return true;
	}

	/* drop invalid entries */
	ctx->cached_sq_head++;
	ring->dropped++;
	/* See comment at the top of this file */
	smp_wmb();
	return false;
}

static int io_ring_submit(struct io_ring_ctx *ctx, unsigned int to_submit)
{
	struct io_submit_state state, *statep = NULL;
	int i, ret = 0, submit = 0;

	if (to_submit > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, to_submit);
		statep = &state;
	}

	for (i = 0; i < to_submit; i++) {
		struct sqe_submit s;

		if (!io_get_sqring(ctx, &s))
			break;

		s.has_user = true;
		s.needs_lock = false;

		ret = io_submit_sqe(ctx, &s, statep);
		if (ret) {
			io_drop_sqring(ctx);
			break;
		}

		submit++;
	}
	io_commit_sqring(ctx);

	if (statep)
		io_submit_state_end(statep);

	return submit ? submit : ret;
}

static unsigned io_cqring_events(struct io_cq_ring *ring)
{
	return READ_ONCE(ring->r.tail) - READ_ONCE(ring->r.head);
}

/*
 * Wait until events become available, if we don't already have some. The
 * application must reap them itself, as they reside on the shared cq ring.
 */
static int io_cqring_wait(struct io_ring_ctx *ctx, int min_events,
			  const sigset_t __user *sig, size_t sigsz)
{
	struct io_cq_ring *ring = ctx->cq_ring;
	sigset_t ksigmask, sigsaved;
	DEFINE_WAIT(wait);
	int ret;

	/* See comment at the top of this file */
	smp_rmb();
	if (io_cqring_events(ring) >= min_events)
		return 0;

	if (sig) {
		ret = set_user_sigmask(sig, &ksigmask, &sigsaved, sigsz);
		if (ret)
			return ret;
	}

	do {
		prepare_to_wait(&ctx->wait, &wait, TASK_INTERRUPTIBLE);

		ret = 0;
		/* See comment at the top of this file */
		smp_rmb();
		if (io_cqring_events(ring) >= min_events)
			break;

		schedule();

		ret = -EINTR;
		if (signal_pending(current))
			break;
	} while (1);

	finish_wait(&ctx->wait, &wait);

	if (sig)
		restore_user_sigmask(sig, &sigsaved);

	return READ_ONCE(ring->r.head) == READ_ONCE(ring->r.tail) ? ret : 0;
}

static int io_sq_offload_start(struct io_ring_ctx *ctx)
{
	int ret;

	mmgrab(current->mm);
	ctx->sqo_mm = current->mm;

	/* Do QD, or 2 * CPUS, whatever is smallest */
	ctx->sqo_wq = alloc_workqueue("io_ring-wq", WQ_UNBOUND | WQ_FREEZABLE,
			min(ctx->sq_entries - 1, 2 * num_online_cpus()));
	if (!ctx->sqo_wq) {
		ret = -ENOMEM;
		goto err;
	}

	return 0;
err:
	mmdrop(ctx->sqo_mm);
	ctx->sqo_mm = NULL;
	return ret;
}

static void io_unaccount_mem(struct user_struct *user, unsigned long nr_pages)
{
	atomic_long_sub(nr_pages, &user->locked_vm);
}

static int io_account_mem(struct user_struct *user, unsigned long nr_pages)
{
	unsigned long page_limit, cur_pages, new_pages;

	/* Don't allow more pages than we can safely lock */
	page_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	do {
		cur_pages = atomic_long_read(&user->locked_vm);
		new_pages = cur_pages + nr_pages;
		if (new_pages > page_limit)
			return -ENOMEM;
	} while (atomic_long_cmpxchg(&user->locked_vm, cur_pages,
					new_pages) != cur_pages);

	return 0;
}

static void io_mem_free(void *ptr)
{
	struct page *page = virt_to_head_page(ptr);

	if (put_page_testzero(page))
		free_compound_page(page);
}

static void *io_mem_alloc(size_t size)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN | __GFP_COMP |
				__GFP_NORETRY;

	return (void *) __get_free_pages(gfp_flags, get_order(size));
}

static unsigned long ring_pages(unsigned sq_entries, unsigned cq_entries)
{
	struct io_sq_ring *sq_ring;
	struct io_cq_ring *cq_ring;
	size_t bytes;

	bytes = struct_size(sq_ring, array, sq_entries);
	bytes += array_size(sizeof(struct io_uring_sqe), sq_entries);
	bytes += struct_size(cq_ring, cqes, cq_entries);

	return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void io_ring_ctx_free(struct io_ring_ctx *ctx)
{
	if (ctx->sqo_wq)
		destroy_workqueue(ctx->sqo_wq);
	if (ctx->sqo_mm)
		mmdrop(ctx->sqo_mm);

	io_iopoll_reap_events(ctx);

#if defined(CONFIG_UNIX)
	if (ctx->ring_sock)
		sock_release(ctx->ring_sock);
#endif

	io_mem_free(ctx->sq_ring);
	io_mem_free(ctx->sq_sqes);
	io_mem_free(ctx->cq_ring);

	percpu_ref_exit(&ctx->refs);
	if (ctx->account_mem)
		io_unaccount_mem(ctx->user,
				ring_pages(ctx->sq_entries, ctx->cq_entries));
	free_uid(ctx->user);
	kfree(ctx);
}

static __poll_t io_uring_poll(struct file *file, poll_table *wait)
{
	struct io_ring_ctx *ctx = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctx->cq_wait, wait);
	/* See comment at the top of this file */
	smp_rmb();
	if (READ_ONCE(ctx->sq_ring->r.tail) + 1 != ctx->cached_sq_head)
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (READ_ONCE(ctx->cq_ring->r.head) != ctx->cached_cq_tail)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int io_uring_fasync(int fd, struct file *file, int on)
{
	struct io_ring_ctx *ctx = file->private_data;

	return fasync_helper(fd, file, on, &ctx->cq_fasync);
}

static void io_ring_ctx_wait_and_kill(struct io_ring_ctx *ctx)
{
	mutex_lock(&ctx->uring_lock);
	percpu_ref_kill(&ctx->refs);
	mutex_unlock(&ctx->uring_lock);

	io_iopoll_reap_events(ctx);
	wait_for_completion(&ctx->ctx_done);
	io_ring_ctx_free(ctx);
}

static int io_uring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	io_ring_ctx_wait_and_kill(ctx);
	return 0;
}

static int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	loff_t offset = (loff_t) vma->vm_pgoff << PAGE_SHIFT;
	unsigned long sz = vma->vm_end - vma->vm_start;
	struct io_ring_ctx *ctx = file->private_data;
	unsigned long pfn;
	struct page *page;
	void *ptr;

	switch (offset) {
	case IORING_OFF_SQ_RING:
		ptr = ctx->sq_ring;
		break;
	case IORING_OFF_SQES:
		ptr = ctx->sq_sqes;
		break;
	case IORING_OFF_CQ_RING:
		ptr = ctx->cq_ring;
		break;
	default:
		return -EINVAL;
	}

	page = virt_to_head_page(ptr);
	if (sz > (PAGE_SIZE << compound_order(page)))
		return -EINVAL;

	pfn = virt_to_phys(ptr) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn, sz, vma->vm_page_prot);
}

SYSCALL_DEFINE6(io_uring_enter, unsigned int, fd, u32, to_submit,
		u32, min_complete, u32, flags, const sigset_t __user *, sig,
		size_t, sigsz)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	int submitted = 0;
	struct fd f;

	if (flags & ~IORING_ENTER_GETEVENTS)
		return -EINVAL;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_uring_fops)
		goto out_fput;

	ret = -ENXIO;
	ctx = f.file->private_data;
	if (!percpu_ref_tryget(&ctx->refs))
		goto out_fput;

	ret = 0;
	if (to_submit) {
		to_submit = min(to_submit, ctx->sq_entries);

		mutex_lock(&ctx->uring_lock);
		submitted = io_ring_submit(ctx, to_submit);
		mutex_unlock(&ctx->uring_lock);

		if (submitted < 0)
			goto out_ctx;
	}
	if (flags & IORING_ENTER_GETEVENTS) {
		unsigned nr_events = 0;

		min_complete = min(min_complete, ctx->cq_entries);

		/*
		 * The application could have included the 'to_submit' count
		 * in how many events it wanted to wait for. If we failed to
		 * submit the desired count, we may need to adjust the number
		 * of events to poll/wait for.
		 */
		if (submitted < to_submit)
			min_complete = min_t(unsigned, submitted, min_complete);

		if (ctx->flags & IORING_SETUP_IOPOLL) {
			mutex_lock(&ctx->uring_lock);
			ret = io_iopoll_check(ctx, &nr_events, min_complete);
			mutex_unlock(&ctx->uring_lock);
		} else {
			ret = io_cqring_wait(ctx, min_complete, sig, sigsz);
		}
	}

out_ctx:
	io_ring_drop_ctx_refs(ctx, 1);
out_fput:
	fdput(f);
	return submitted ? submitted : ret;
}

static const struct file_operations io_uring_fops = {
	.release	= io_uring_release,
	.mmap		= io_uring_mmap,
	.poll		= io_uring_poll,
	.fasync		= io_uring_fasync,
};

static int io_allocate_scq_urings(struct io_ring_ctx *ctx,
				  struct io_uring_params *p)
{
	struct io_sq_ring *sq_ring;
	struct io_cq_ring *cq_ring;
	size_t size;

	sq_ring = io_mem_alloc(struct_size(sq_ring, array, p->sq_entries));
	if (!sq_ring)
		return -ENOMEM;

	ctx->sq_ring = sq_ring;
	sq_ring->ring_mask = p->sq_entries - 1;
	sq_ring->ring_entries = p->sq_entries;
	ctx->sq_mask = sq_ring->ring_mask;
	ctx->sq_entries = sq_ring->ring_entries;

	size = array_size(sizeof(struct io_uring_sqe), p->sq_entries);
	if (size == SIZE_MAX)
		return -EOVERFLOW;

	ctx->sq_sqes = io_mem_alloc(size);
	if (!ctx->sq_sqes) {
		io_mem_free(ctx->sq_ring);
		return -ENOMEM;
	}

	cq_ring = io_mem_alloc(struct_size(cq_ring, cqes, p->cq_entries));
	if (!cq_ring) {
		io_mem_free(ctx->sq_ring);
		io_mem_free(ctx->sq_sqes);
		return -ENOMEM;
	}

	ctx->cq_ring = cq_ring;
	cq_ring->ring_mask = p->cq_entries - 1;
	cq_ring->ring_entries = p->cq_entries;
	ctx->cq_mask = cq_ring->ring_mask;
	ctx->cq_entries = cq_ring->ring_entries;
	return 0;
}

/*
 * Allocate an anonymous fd, this is what constitutes the application
 * visible backing of an io_uring instance. The application mmaps this
 * fd to gain access to the SQ/CQ ring details. If UNIX sockets are enabled,
 * we have to tie this fd to a socket for file garbage collection purposes.
 */
static int io_uring_get_fd(struct io_ring_ctx *ctx)
{
	struct file *file;
	int ret;

#if defined(CONFIG_UNIX)
	ret = sock_create_kern(&init_net, PF_UNIX, SOCK_RAW, IPPROTO_IP,
				&ctx->ring_sock);
	if (ret)
		return ret;
#endif

	ret = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto err;

	file = anon_inode_getfile("[io_uring]", &io_uring_fops, ctx,
					O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		put_unused_fd(ret);
		ret = PTR_ERR(file);
		goto err;
	}

#if defined(CONFIG_UNIX)
	ctx->ring_sock->file = file;
#endif
	fd_install(ret, file);
	return ret;
err:
#if defined(CONFIG_UNIX)
	sock_release(ctx->ring_sock);
	ctx->ring_sock = NULL;
#endif
	return ret;
}

static int io_uring_create(unsigned entries, struct io_uring_params *p)
{
	struct user_struct *user = NULL;
	struct io_ring_ctx *ctx;
	bool account_mem;
	int ret;

	if (!entries || entries > IORING_MAX_ENTRIES)
		return -EINVAL;

	/*
	 * Use twice as many entries for the CQ ring. It's possible for the
	 * application to drive a higher depth than the size of the SQ ring,
	 * since the sqes are only used at submission time. This allows for
	 * some flexibility in overcommitting a bit.
	 */
	p->sq_entries = roundup_pow_of_two(entries);
	p->cq_entries = 2 * p->sq_entries;

	user = get_uid(current_user());
	account_mem = !capable(CAP_IPC_LOCK);

	if (account_mem) {
		ret = io_account_mem(user,
				ring_pages(p->sq_entries, p->cq_entries));
		if (ret) {
			free_uid(user);
			return ret;
		}
	}

	ctx = io_ring_ctx_alloc(p);
	if (!ctx) {
		if (account_mem)
			io_unaccount_mem(user, ring_pages(p->sq_entries,
								p->cq_entries));
		free_uid(user);
		return -ENOMEM;
	}
	ctx->compat = in_compat_syscall();
	ctx->account_mem = account_mem;
	ctx->user = user;

	ret = io_allocate_scq_urings(ctx, p);
	if (ret)
		goto err;

	ret = io_sq_offload_start(ctx);
	if (ret)
		goto err;

	ret = io_uring_get_fd(ctx);
	if (ret < 0)
		goto err;

	memset(&p->sq_off, 0, sizeof(p->sq_off));
	p->sq_off.head = offsetof(struct io_sq_ring, r.head);
	p->sq_off.tail = offsetof(struct io_sq_ring, r.tail);
	p->sq_off.ring_mask = offsetof(struct io_sq_ring, ring_mask);
	p->sq_off.ring_entries = offsetof(struct io_sq_ring, ring_entries);
	p->sq_off.flags = offsetof(struct io_sq_ring, flags);
	p->sq_off.dropped = offsetof(struct io_sq_ring, dropped);
	p->sq_off.array = offsetof(struct io_sq_ring, array);

	memset(&p->cq_off, 0, sizeof(p->cq_off));
	p->cq_off.head = offsetof(struct io_cq_ring, r.head);
	p->cq_off.tail = offsetof(struct io_cq_ring, r.tail);
	p->cq_off.ring_mask = offsetof(struct io_cq_ring, ring_mask);
	p->cq_off.ring_entries = offsetof(struct io_cq_ring, ring_entries);
	p->cq_off.overflow = offsetof(struct io_cq_ring, overflow);
	p->cq_off.cqes = offsetof(struct io_cq_ring, cqes);
	return ret;
err:
	io_ring_ctx_wait_and_kill(ctx);
	return ret;
}

/*
 * Sets up an aio uring context, and returns the fd. Applications asks for a
 * ring size, we return the actual sq/cq ring sizes (among other things) in the
 * params structure passed in.
 */
static long io_uring_setup(u32 entries, struct io_uring_params __user *params)
{
	struct io_uring_params p;
	long ret;
	int i;

	if (copy_from_user(&p, params, sizeof(p)))
		return -EFAULT;
	for (i = 0; i < ARRAY_SIZE(p.resv); i++) {
		if (p.resv[i])
			return -EINVAL;
	}

	if (p.flags & ~IORING_SETUP_IOPOLL)
		return -EINVAL;

	ret = io_uring_create(entries, &p);
	if (ret < 0)
		return ret;

	if (copy_to_user(params, &p, sizeof(p)))
		return -EFAULT;

	return ret;
}

SYSCALL_DEFINE2(io_uring_setup, u32, entries,
		struct io_uring_params __user *, params)
{
	return io_uring_setup(entries, params);
}

static int __init io_uring_init(void)
{
	req_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return 0;
};
__initcall(io_uring_init);
