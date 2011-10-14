/*
   drbd_req.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/module.h>

#include <linux/slab.h>
#include <linux/drbd.h>
#include "drbd_int.h"
#include "drbd_req.h"


/* Update disk stats at start of I/O request */
static void _drbd_start_io_acct(struct drbd_conf *mdev, struct drbd_request *req, struct bio *bio)
{
	const int rw = bio_data_dir(bio);
	int cpu;
	cpu = part_stat_lock();
	part_stat_inc(cpu, &mdev->vdisk->part0, ios[rw]);
	part_stat_add(cpu, &mdev->vdisk->part0, sectors[rw], bio_sectors(bio));
	part_inc_in_flight(&mdev->vdisk->part0, rw);
	part_stat_unlock();
}

/* Update disk stats when completing request upwards */
static void _drbd_end_io_acct(struct drbd_conf *mdev, struct drbd_request *req)
{
	int rw = bio_data_dir(req->master_bio);
	unsigned long duration = jiffies - req->start_time;
	int cpu;
	cpu = part_stat_lock();
	part_stat_add(cpu, &mdev->vdisk->part0, ticks[rw], duration);
	part_round_stats(cpu, &mdev->vdisk->part0);
	part_dec_in_flight(&mdev->vdisk->part0, rw);
	part_stat_unlock();
}

static struct drbd_request *drbd_req_new(struct drbd_conf *mdev,
					       struct bio *bio_src)
{
	struct drbd_request *req;

	req = mempool_alloc(drbd_request_mempool, GFP_NOIO);
	if (!req)
		return NULL;

	drbd_req_make_private_bio(req, bio_src);
	req->rq_state    = bio_data_dir(bio_src) == WRITE ? RQ_WRITE : 0;
	req->w.mdev      = mdev;
	req->master_bio  = bio_src;
	req->epoch       = 0;

	drbd_clear_interval(&req->i);
	req->i.sector     = bio_src->bi_sector;
	req->i.size      = bio_src->bi_size;
	req->i.local = true;
	req->i.waiting = false;

	INIT_LIST_HEAD(&req->tl_requests);
	INIT_LIST_HEAD(&req->w.list);

	return req;
}

static void drbd_req_free(struct drbd_request *req)
{
	mempool_free(req, drbd_request_mempool);
}

/* rw is bio_data_dir(), only READ or WRITE */
static void _req_is_done(struct drbd_conf *mdev, struct drbd_request *req, const int rw)
{
	const unsigned long s = req->rq_state;

	/* remove it from the transfer log.
	 * well, only if it had been there in the first
	 * place... if it had not (local only or conflicting
	 * and never sent), it should still be "empty" as
	 * initialized in drbd_req_new(), so we can list_del() it
	 * here unconditionally */
	list_del(&req->tl_requests);

	/* if it was a write, we may have to set the corresponding
	 * bit(s) out-of-sync first. If it had a local part, we need to
	 * release the reference to the activity log. */
	if (rw == WRITE) {
		/* Set out-of-sync unless both OK flags are set
		 * (local only or remote failed).
		 * Other places where we set out-of-sync:
		 * READ with local io-error */
		if (!(s & RQ_NET_OK) || !(s & RQ_LOCAL_OK))
			drbd_set_out_of_sync(mdev, req->i.sector, req->i.size);

		if ((s & RQ_NET_OK) && (s & RQ_LOCAL_OK) && (s & RQ_NET_SIS))
			drbd_set_in_sync(mdev, req->i.sector, req->i.size);

		/* one might be tempted to move the drbd_al_complete_io
		 * to the local io completion callback drbd_request_endio.
		 * but, if this was a mirror write, we may only
		 * drbd_al_complete_io after this is RQ_NET_DONE,
		 * otherwise the extent could be dropped from the al
		 * before it has actually been written on the peer.
		 * if we crash before our peer knows about the request,
		 * but after the extent has been dropped from the al,
		 * we would forget to resync the corresponding extent.
		 */
		if (s & RQ_LOCAL_MASK) {
			if (get_ldev_if_state(mdev, D_FAILED)) {
				if (s & RQ_IN_ACT_LOG)
					drbd_al_complete_io(mdev, req->i.sector);
				put_ldev(mdev);
			} else if (__ratelimit(&drbd_ratelimit_state)) {
				dev_warn(DEV, "Should have called drbd_al_complete_io(, %llu), "
				     "but my Disk seems to have failed :(\n",
				     (unsigned long long) req->i.sector);
			}
		}
	}

	drbd_req_free(req);
}

static void queue_barrier(struct drbd_conf *mdev)
{
	struct drbd_tl_epoch *b;

	/* We are within the req_lock. Once we queued the barrier for sending,
	 * we set the CREATE_BARRIER bit. It is cleared as soon as a new
	 * barrier/epoch object is added. This is the only place this bit is
	 * set. It indicates that the barrier for this epoch is already queued,
	 * and no new epoch has been created yet. */
	if (test_bit(CREATE_BARRIER, &mdev->flags))
		return;

	b = mdev->tconn->newest_tle;
	b->w.cb = w_send_barrier;
	b->w.mdev = mdev;
	/* inc_ap_pending done here, so we won't
	 * get imbalanced on connection loss.
	 * dec_ap_pending will be done in got_BarrierAck
	 * or (on connection loss) in tl_clear.  */
	inc_ap_pending(mdev);
	drbd_queue_work(&mdev->tconn->data.work, &b->w);
	set_bit(CREATE_BARRIER, &mdev->flags);
}

static void _about_to_complete_local_write(struct drbd_conf *mdev,
	struct drbd_request *req)
{
	const unsigned long s = req->rq_state;

	/* Before we can signal completion to the upper layers,
	 * we may need to close the current epoch.
	 * We can skip this, if this request has not even been sent, because we
	 * did not have a fully established connection yet/anymore, during
	 * bitmap exchange, or while we are C_AHEAD due to congestion policy.
	 */
	if (mdev->state.conn >= C_CONNECTED &&
	    (s & RQ_NET_SENT) != 0 &&
	    req->epoch == mdev->tconn->newest_tle->br_number)
		queue_barrier(mdev);
}

void complete_master_bio(struct drbd_conf *mdev,
		struct bio_and_error *m)
{
	bio_endio(m->bio, m->error);
	dec_ap_bio(mdev);
}


static void drbd_remove_request_interval(struct rb_root *root,
					 struct drbd_request *req)
{
	struct drbd_conf *mdev = req->w.mdev;
	struct drbd_interval *i = &req->i;

	drbd_remove_interval(root, i);

	/* Wake up any processes waiting for this request to complete.  */
	if (i->waiting)
		wake_up(&mdev->misc_wait);
}

/* Helper for __req_mod().
 * Set m->bio to the master bio, if it is fit to be completed,
 * or leave it alone (it is initialized to NULL in __req_mod),
 * if it has already been completed, or cannot be completed yet.
 * If m->bio is set, the error status to be returned is placed in m->error.
 */
void _req_may_be_done(struct drbd_request *req, struct bio_and_error *m)
{
	const unsigned long s = req->rq_state;
	struct drbd_conf *mdev = req->w.mdev;
	/* only WRITES may end up here without a master bio (on barrier ack) */
	int rw = req->master_bio ? bio_data_dir(req->master_bio) : WRITE;

	/* we must not complete the master bio, while it is
	 *	still being processed by _drbd_send_zc_bio (drbd_send_dblock)
	 *	not yet acknowledged by the peer
	 *	not yet completed by the local io subsystem
	 * these flags may get cleared in any order by
	 *	the worker,
	 *	the receiver,
	 *	the bio_endio completion callbacks.
	 */
	if (s & RQ_LOCAL_PENDING)
		return;
	if (req->i.waiting) {
		/* Retry all conflicting peer requests.  */
		wake_up(&mdev->misc_wait);
	}
	if (s & RQ_NET_QUEUED)
		return;
	if (s & RQ_NET_PENDING)
		return;

	if (req->master_bio) {
		/* this is DATA_RECEIVED (remote read)
		 * or protocol C P_WRITE_ACK
		 * or protocol B P_RECV_ACK
		 * or protocol A "HANDED_OVER_TO_NETWORK" (SendAck)
		 * or canceled or failed,
		 * or killed from the transfer log due to connection loss.
		 */

		/*
		 * figure out whether to report success or failure.
		 *
		 * report success when at least one of the operations succeeded.
		 * or, to put the other way,
		 * only report failure, when both operations failed.
		 *
		 * what to do about the failures is handled elsewhere.
		 * what we need to do here is just: complete the master_bio.
		 *
		 * local completion error, if any, has been stored as ERR_PTR
		 * in private_bio within drbd_request_endio.
		 */
		int ok = (s & RQ_LOCAL_OK) || (s & RQ_NET_OK);
		int error = PTR_ERR(req->private_bio);

		/* remove the request from the conflict detection
		 * respective block_id verification hash */
		if (!drbd_interval_empty(&req->i)) {
			struct rb_root *root;

			if (rw == WRITE)
				root = &mdev->write_requests;
			else
				root = &mdev->read_requests;
			drbd_remove_request_interval(root, req);
		} else if (!(s & RQ_POSTPONED))
			D_ASSERT((s & (RQ_NET_MASK & ~RQ_NET_DONE)) == 0);

		/* for writes we need to do some extra housekeeping */
		if (rw == WRITE)
			_about_to_complete_local_write(mdev, req);

		/* Update disk stats */
		_drbd_end_io_acct(mdev, req);

		if (!(s & RQ_POSTPONED)) {
			m->error = ok ? 0 : (error ?: -EIO);
			m->bio = req->master_bio;
		}
		req->master_bio = NULL;
	}

	if ((s & RQ_NET_MASK) == 0 || (s & RQ_NET_DONE)) {
		/* this is disconnected (local only) operation,
		 * or protocol C P_WRITE_ACK,
		 * or protocol A or B P_BARRIER_ACK,
		 * or killed from the transfer log due to connection loss. */
		_req_is_done(mdev, req, rw);
	}
	/* else: network part and not DONE yet. that is
	 * protocol A or B, barrier ack still pending... */
}

static void _req_may_be_done_not_susp(struct drbd_request *req, struct bio_and_error *m)
{
	struct drbd_conf *mdev = req->w.mdev;

	if (!is_susp(mdev->state))
		_req_may_be_done(req, m);
}

/* obviously this could be coded as many single functions
 * instead of one huge switch,
 * or by putting the code directly in the respective locations
 * (as it has been before).
 *
 * but having it this way
 *  enforces that it is all in this one place, where it is easier to audit,
 *  it makes it obvious that whatever "event" "happens" to a request should
 *  happen "atomically" within the req_lock,
 *  and it enforces that we have to think in a very structured manner
 *  about the "events" that may happen to a request during its life time ...
 */
int __req_mod(struct drbd_request *req, enum drbd_req_event what,
		struct bio_and_error *m)
{
	struct drbd_conf *mdev = req->w.mdev;
	int rv = 0;

	if (m)
		m->bio = NULL;

	switch (what) {
	default:
		dev_err(DEV, "LOGIC BUG in %s:%u\n", __FILE__ , __LINE__);
		break;

	/* does not happen...
	 * initialization done in drbd_req_new
	case CREATED:
		break;
		*/

	case TO_BE_SENT: /* via network */
		/* reached via __drbd_make_request
		 * and from w_read_retry_remote */
		D_ASSERT(!(req->rq_state & RQ_NET_MASK));
		req->rq_state |= RQ_NET_PENDING;
		inc_ap_pending(mdev);
		break;

	case TO_BE_SUBMITTED: /* locally */
		/* reached via __drbd_make_request */
		D_ASSERT(!(req->rq_state & RQ_LOCAL_MASK));
		req->rq_state |= RQ_LOCAL_PENDING;
		break;

	case COMPLETED_OK:
		if (bio_data_dir(req->master_bio) == WRITE)
			mdev->writ_cnt += req->i.size >> 9;
		else
			mdev->read_cnt += req->i.size >> 9;

		req->rq_state |= (RQ_LOCAL_COMPLETED|RQ_LOCAL_OK);
		req->rq_state &= ~RQ_LOCAL_PENDING;

		_req_may_be_done_not_susp(req, m);
		put_ldev(mdev);
		break;

	case WRITE_COMPLETED_WITH_ERROR:
		req->rq_state |= RQ_LOCAL_COMPLETED;
		req->rq_state &= ~RQ_LOCAL_PENDING;

		__drbd_chk_io_error(mdev, false);
		_req_may_be_done_not_susp(req, m);
		put_ldev(mdev);
		break;

	case READ_AHEAD_COMPLETED_WITH_ERROR:
		/* it is legal to fail READA */
		req->rq_state |= RQ_LOCAL_COMPLETED;
		req->rq_state &= ~RQ_LOCAL_PENDING;
		_req_may_be_done_not_susp(req, m);
		put_ldev(mdev);
		break;

	case READ_COMPLETED_WITH_ERROR:
		drbd_set_out_of_sync(mdev, req->i.sector, req->i.size);

		req->rq_state |= RQ_LOCAL_COMPLETED;
		req->rq_state &= ~RQ_LOCAL_PENDING;

		D_ASSERT(!(req->rq_state & RQ_NET_MASK));

		__drbd_chk_io_error(mdev, false);
		put_ldev(mdev);

		/* no point in retrying if there is no good remote data,
		 * or we have no connection. */
		if (mdev->state.pdsk != D_UP_TO_DATE) {
			_req_may_be_done_not_susp(req, m);
			break;
		}

		/* _req_mod(req,TO_BE_SENT); oops, recursion... */
		req->rq_state |= RQ_NET_PENDING;
		inc_ap_pending(mdev);
		/* fall through: _req_mod(req,QUEUE_FOR_NET_READ); */

	case QUEUE_FOR_NET_READ:
		/* READ or READA, and
		 * no local disk,
		 * or target area marked as invalid,
		 * or just got an io-error. */
		/* from __drbd_make_request
		 * or from bio_endio during read io-error recovery */

		/* so we can verify the handle in the answer packet
		 * corresponding hlist_del is in _req_may_be_done() */
		drbd_insert_interval(&mdev->read_requests, &req->i);

		set_bit(UNPLUG_REMOTE, &mdev->flags);

		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		req->rq_state |= RQ_NET_QUEUED;
		req->w.cb = (req->rq_state & RQ_LOCAL_MASK)
			? w_read_retry_remote
			: w_send_read_req;
		drbd_queue_work(&mdev->tconn->data.work, &req->w);
		break;

	case QUEUE_FOR_NET_WRITE:
		/* assert something? */
		/* from __drbd_make_request only */

		/* corresponding hlist_del is in _req_may_be_done() */
		drbd_insert_interval(&mdev->write_requests, &req->i);

		/* NOTE
		 * In case the req ended up on the transfer log before being
		 * queued on the worker, it could lead to this request being
		 * missed during cleanup after connection loss.
		 * So we have to do both operations here,
		 * within the same lock that protects the transfer log.
		 *
		 * _req_add_to_epoch(req); this has to be after the
		 * _maybe_start_new_epoch(req); which happened in
		 * __drbd_make_request, because we now may set the bit
		 * again ourselves to close the current epoch.
		 *
		 * Add req to the (now) current epoch (barrier). */

		/* otherwise we may lose an unplug, which may cause some remote
		 * io-scheduler timeout to expire, increasing maximum latency,
		 * hurting performance. */
		set_bit(UNPLUG_REMOTE, &mdev->flags);

		/* see __drbd_make_request,
		 * just after it grabs the req_lock */
		D_ASSERT(test_bit(CREATE_BARRIER, &mdev->flags) == 0);

		req->epoch = mdev->tconn->newest_tle->br_number;

		/* increment size of current epoch */
		mdev->tconn->newest_tle->n_writes++;

		/* queue work item to send data */
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		req->rq_state |= RQ_NET_QUEUED;
		req->w.cb =  w_send_dblock;
		drbd_queue_work(&mdev->tconn->data.work, &req->w);

		/* close the epoch, in case it outgrew the limit */
		if (mdev->tconn->newest_tle->n_writes >= mdev->tconn->net_conf->max_epoch_size)
			queue_barrier(mdev);

		break;

	case QUEUE_FOR_SEND_OOS:
		req->rq_state |= RQ_NET_QUEUED;
		req->w.cb =  w_send_oos;
		drbd_queue_work(&mdev->tconn->data.work, &req->w);
		break;

	case OOS_HANDED_TO_NETWORK:
		/* actually the same */
	case SEND_CANCELED:
		/* treat it the same */
	case SEND_FAILED:
		/* real cleanup will be done from tl_clear.  just update flags
		 * so it is no longer marked as on the worker queue */
		req->rq_state &= ~RQ_NET_QUEUED;
		/* if we did it right, tl_clear should be scheduled only after
		 * this, so this should not be necessary! */
		_req_may_be_done_not_susp(req, m);
		break;

	case HANDED_OVER_TO_NETWORK:
		/* assert something? */
		if (bio_data_dir(req->master_bio) == WRITE)
			atomic_add(req->i.size >> 9, &mdev->ap_in_flight);

		if (bio_data_dir(req->master_bio) == WRITE &&
		    mdev->tconn->net_conf->wire_protocol == DRBD_PROT_A) {
			/* this is what is dangerous about protocol A:
			 * pretend it was successfully written on the peer. */
			if (req->rq_state & RQ_NET_PENDING) {
				dec_ap_pending(mdev);
				req->rq_state &= ~RQ_NET_PENDING;
				req->rq_state |= RQ_NET_OK;
			} /* else: neg-ack was faster... */
			/* it is still not yet RQ_NET_DONE until the
			 * corresponding epoch barrier got acked as well,
			 * so we know what to dirty on connection loss */
		}
		req->rq_state &= ~RQ_NET_QUEUED;
		req->rq_state |= RQ_NET_SENT;
		/* because _drbd_send_zc_bio could sleep, and may want to
		 * dereference the bio even after the "WRITE_ACKED_BY_PEER" and
		 * "COMPLETED_OK" events came in, once we return from
		 * _drbd_send_zc_bio (drbd_send_dblock), we have to check
		 * whether it is done already, and end it.  */
		_req_may_be_done_not_susp(req, m);
		break;

	case READ_RETRY_REMOTE_CANCELED:
		req->rq_state &= ~RQ_NET_QUEUED;
		/* fall through, in case we raced with drbd_disconnect */
	case CONNECTION_LOST_WHILE_PENDING:
		/* transfer log cleanup after connection loss */
		/* assert something? */
		if (req->rq_state & RQ_NET_PENDING)
			dec_ap_pending(mdev);
		req->rq_state &= ~(RQ_NET_OK|RQ_NET_PENDING);
		req->rq_state |= RQ_NET_DONE;
		if (req->rq_state & RQ_NET_SENT && req->rq_state & RQ_WRITE)
			atomic_sub(req->i.size >> 9, &mdev->ap_in_flight);

		/* if it is still queued, we may not complete it here.
		 * it will be canceled soon. */
		if (!(req->rq_state & RQ_NET_QUEUED))
			_req_may_be_done(req, m); /* Allowed while state.susp */
		break;

	case WRITE_ACKED_BY_PEER_AND_SIS:
		req->rq_state |= RQ_NET_SIS;
	case DISCARD_WRITE:
		/* for discarded conflicting writes of multiple primaries,
		 * there is no need to keep anything in the tl, potential
		 * node crashes are covered by the activity log. */
		req->rq_state |= RQ_NET_DONE;
		/* fall through */
	case WRITE_ACKED_BY_PEER:
		/* protocol C; successfully written on peer.
		 * Nothing to do here.
		 * We want to keep the tl in place for all protocols, to cater
		 * for volatile write-back caches on lower level devices.
		 *
		 * A barrier request is expected to have forced all prior
		 * requests onto stable storage, so completion of a barrier
		 * request could set NET_DONE right here, and not wait for the
		 * P_BARRIER_ACK, but that is an unnecessary optimization. */

		/* this makes it effectively the same as for: */
	case RECV_ACKED_BY_PEER:
		/* protocol B; pretends to be successfully written on peer.
		 * see also notes above in HANDED_OVER_TO_NETWORK about
		 * protocol != C */
		req->rq_state |= RQ_NET_OK;
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		dec_ap_pending(mdev);
		atomic_sub(req->i.size >> 9, &mdev->ap_in_flight);
		req->rq_state &= ~RQ_NET_PENDING;
		_req_may_be_done_not_susp(req, m);
		break;

	case POSTPONE_WRITE:
		/*
		 * If this node has already detected the write conflict, the
		 * worker will be waiting on misc_wait.  Wake it up once this
		 * request has completed locally.
		 */
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		req->rq_state |= RQ_POSTPONED;
		_req_may_be_done_not_susp(req, m);
		break;

	case NEG_ACKED:
		/* assert something? */
		if (req->rq_state & RQ_NET_PENDING) {
			dec_ap_pending(mdev);
			atomic_sub(req->i.size >> 9, &mdev->ap_in_flight);
		}
		req->rq_state &= ~(RQ_NET_OK|RQ_NET_PENDING);

		req->rq_state |= RQ_NET_DONE;
		_req_may_be_done_not_susp(req, m);
		/* else: done by HANDED_OVER_TO_NETWORK */
		break;

	case FAIL_FROZEN_DISK_IO:
		if (!(req->rq_state & RQ_LOCAL_COMPLETED))
			break;

		_req_may_be_done(req, m); /* Allowed while state.susp */
		break;

	case RESTART_FROZEN_DISK_IO:
		if (!(req->rq_state & RQ_LOCAL_COMPLETED))
			break;

		req->rq_state &= ~RQ_LOCAL_COMPLETED;

		rv = MR_READ;
		if (bio_data_dir(req->master_bio) == WRITE)
			rv = MR_WRITE;

		get_ldev(mdev);
		req->w.cb = w_restart_disk_io;
		drbd_queue_work(&mdev->tconn->data.work, &req->w);
		break;

	case RESEND:
		/* If RQ_NET_OK is already set, we got a P_WRITE_ACK or P_RECV_ACK
		   before the connection loss (B&C only); only P_BARRIER_ACK was missing.
		   Trowing them out of the TL here by pretending we got a BARRIER_ACK
		   We ensure that the peer was not rebooted */
		if (!(req->rq_state & RQ_NET_OK)) {
			if (req->w.cb) {
				drbd_queue_work(&mdev->tconn->data.work, &req->w);
				rv = req->rq_state & RQ_WRITE ? MR_WRITE : MR_READ;
			}
			break;
		}
		/* else, fall through to BARRIER_ACKED */

	case BARRIER_ACKED:
		if (!(req->rq_state & RQ_WRITE))
			break;

		if (req->rq_state & RQ_NET_PENDING) {
			/* barrier came in before all requests have been acked.
			 * this is bad, because if the connection is lost now,
			 * we won't be able to clean them up... */
			dev_err(DEV, "FIXME (BARRIER_ACKED but pending)\n");
			list_move(&req->tl_requests, &mdev->tconn->out_of_sequence_requests);
		}
		if ((req->rq_state & RQ_NET_MASK) != 0) {
			req->rq_state |= RQ_NET_DONE;
			if (mdev->tconn->net_conf->wire_protocol == DRBD_PROT_A)
				atomic_sub(req->i.size>>9, &mdev->ap_in_flight);
		}
		_req_may_be_done(req, m); /* Allowed while state.susp */
		break;

	case DATA_RECEIVED:
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		dec_ap_pending(mdev);
		req->rq_state &= ~RQ_NET_PENDING;
		req->rq_state |= (RQ_NET_OK|RQ_NET_DONE);
		_req_may_be_done_not_susp(req, m);
		break;
	};

	return rv;
}

/* we may do a local read if:
 * - we are consistent (of course),
 * - or we are generally inconsistent,
 *   BUT we are still/already IN SYNC for this area.
 *   since size may be bigger than BM_BLOCK_SIZE,
 *   we may need to check several bits.
 */
static int drbd_may_do_local_read(struct drbd_conf *mdev, sector_t sector, int size)
{
	unsigned long sbnr, ebnr;
	sector_t esector, nr_sectors;

	if (mdev->state.disk == D_UP_TO_DATE)
		return 1;
	if (mdev->state.disk != D_INCONSISTENT)
		return 0;
	esector = sector + (size >> 9) - 1;

	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	D_ASSERT(sector  < nr_sectors);
	D_ASSERT(esector < nr_sectors);

	sbnr = BM_SECT_TO_BIT(sector);
	ebnr = BM_SECT_TO_BIT(esector);

	return 0 == drbd_bm_count_bits(mdev, sbnr, ebnr);
}

/*
 * complete_conflicting_writes  -  wait for any conflicting write requests
 *
 * The write_requests tree contains all active write requests which we
 * currently know about.  Wait for any requests to complete which conflict with
 * the new one.
 */
static int complete_conflicting_writes(struct drbd_conf *mdev,
				       sector_t sector, int size)
{
	for(;;) {
		struct drbd_interval *i;
		int err;

		i = drbd_find_overlap(&mdev->write_requests, sector, size);
		if (!i)
			return 0;
		err = drbd_wait_misc(mdev, i);
		if (err)
			return err;
	}
}

int __drbd_make_request(struct drbd_conf *mdev, struct bio *bio, unsigned long start_time)
{
	const int rw = bio_rw(bio);
	const int size = bio->bi_size;
	const sector_t sector = bio->bi_sector;
	struct drbd_tl_epoch *b = NULL;
	struct drbd_request *req;
	int local, remote, send_oos = 0;
	int err;
	int ret = 0;

	/* allocate outside of all locks; */
	req = drbd_req_new(mdev, bio);
	if (!req) {
		dec_ap_bio(mdev);
		/* only pass the error to the upper layers.
		 * if user cannot handle io errors, that's not our business. */
		dev_err(DEV, "could not kmalloc() req\n");
		bio_endio(bio, -ENOMEM);
		return 0;
	}
	req->start_time = start_time;

	local = get_ldev(mdev);
	if (!local) {
		bio_put(req->private_bio); /* or we get a bio leak */
		req->private_bio = NULL;
	}
	if (rw == WRITE) {
		remote = 1;
	} else {
		/* READ || READA */
		if (local) {
			if (!drbd_may_do_local_read(mdev, sector, size)) {
				/* we could kick the syncer to
				 * sync this extent asap, wait for
				 * it, then continue locally.
				 * Or just issue the request remotely.
				 */
				local = 0;
				bio_put(req->private_bio);
				req->private_bio = NULL;
				put_ldev(mdev);
			}
		}
		remote = !local && mdev->state.pdsk >= D_UP_TO_DATE;
	}

	/* If we have a disk, but a READA request is mapped to remote,
	 * we are R_PRIMARY, D_INCONSISTENT, SyncTarget.
	 * Just fail that READA request right here.
	 *
	 * THINK: maybe fail all READA when not local?
	 *        or make this configurable...
	 *        if network is slow, READA won't do any good.
	 */
	if (rw == READA && mdev->state.disk >= D_INCONSISTENT && !local) {
		err = -EWOULDBLOCK;
		goto fail_and_free_req;
	}

	/* For WRITES going to the local disk, grab a reference on the target
	 * extent.  This waits for any resync activity in the corresponding
	 * resync extent to finish, and, if necessary, pulls in the target
	 * extent into the activity log, which involves further disk io because
	 * of transactional on-disk meta data updates. */
	if (rw == WRITE && local && !test_bit(AL_SUSPENDED, &mdev->flags)) {
		req->rq_state |= RQ_IN_ACT_LOG;
		drbd_al_begin_io(mdev, sector);
	}

	remote = remote && drbd_should_do_remote(mdev->state);
	send_oos = rw == WRITE && drbd_should_send_oos(mdev->state);
	D_ASSERT(!(remote && send_oos));

	if (!(local || remote) && !is_susp(mdev->state)) {
		if (__ratelimit(&drbd_ratelimit_state))
			dev_err(DEV, "IO ERROR: neither local nor remote disk\n");
		err = -EIO;
		goto fail_free_complete;
	}

	/* For WRITE request, we have to make sure that we have an
	 * unused_spare_tle, in case we need to start a new epoch.
	 * I try to be smart and avoid to pre-allocate always "just in case",
	 * but there is a race between testing the bit and pointer outside the
	 * spinlock, and grabbing the spinlock.
	 * if we lost that race, we retry.  */
	if (rw == WRITE && (remote || send_oos) &&
	    mdev->tconn->unused_spare_tle == NULL &&
	    test_bit(CREATE_BARRIER, &mdev->flags)) {
allocate_barrier:
		b = kmalloc(sizeof(struct drbd_tl_epoch), GFP_NOIO);
		if (!b) {
			dev_err(DEV, "Failed to alloc barrier.\n");
			err = -ENOMEM;
			goto fail_free_complete;
		}
	}

	/* GOOD, everything prepared, grab the spin_lock */
	spin_lock_irq(&mdev->tconn->req_lock);

	if (rw == WRITE) {
		err = complete_conflicting_writes(mdev, sector, size);
		if (err) {
			if (err != -ERESTARTSYS)
				_conn_request_state(mdev->tconn,
						    NS(conn, C_TIMEOUT),
						    CS_HARD);
			spin_unlock_irq(&mdev->tconn->req_lock);
			err = -EIO;
			goto fail_free_complete;
		}
	}

	if (is_susp(mdev->state)) {
		/* If we got suspended, use the retry mechanism of
		   generic_make_request() to restart processing of this
		   bio. In the next call to drbd_make_request
		   we sleep in inc_ap_bio() */
		ret = 1;
		spin_unlock_irq(&mdev->tconn->req_lock);
		goto fail_free_complete;
	}

	if (remote || send_oos) {
		remote = drbd_should_do_remote(mdev->state);
		send_oos = rw == WRITE && drbd_should_send_oos(mdev->state);
		D_ASSERT(!(remote && send_oos));

		if (!(remote || send_oos))
			dev_warn(DEV, "lost connection while grabbing the req_lock!\n");
		if (!(local || remote)) {
			dev_err(DEV, "IO ERROR: neither local nor remote disk\n");
			spin_unlock_irq(&mdev->tconn->req_lock);
			err = -EIO;
			goto fail_free_complete;
		}
	}

	if (b && mdev->tconn->unused_spare_tle == NULL) {
		mdev->tconn->unused_spare_tle = b;
		b = NULL;
	}
	if (rw == WRITE && (remote || send_oos) &&
	    mdev->tconn->unused_spare_tle == NULL &&
	    test_bit(CREATE_BARRIER, &mdev->flags)) {
		/* someone closed the current epoch
		 * while we were grabbing the spinlock */
		spin_unlock_irq(&mdev->tconn->req_lock);
		goto allocate_barrier;
	}


	/* Update disk stats */
	_drbd_start_io_acct(mdev, req, bio);

	/* _maybe_start_new_epoch(mdev);
	 * If we need to generate a write barrier packet, we have to add the
	 * new epoch (barrier) object, and queue the barrier packet for sending,
	 * and queue the req's data after it _within the same lock_, otherwise
	 * we have race conditions were the reorder domains could be mixed up.
	 *
	 * Even read requests may start a new epoch and queue the corresponding
	 * barrier packet.  To get the write ordering right, we only have to
	 * make sure that, if this is a write request and it triggered a
	 * barrier packet, this request is queued within the same spinlock. */
	if ((remote || send_oos) && mdev->tconn->unused_spare_tle &&
	    test_and_clear_bit(CREATE_BARRIER, &mdev->flags)) {
		_tl_add_barrier(mdev->tconn, mdev->tconn->unused_spare_tle);
		mdev->tconn->unused_spare_tle = NULL;
	} else {
		D_ASSERT(!(remote && rw == WRITE &&
			   test_bit(CREATE_BARRIER, &mdev->flags)));
	}

	/* NOTE
	 * Actually, 'local' may be wrong here already, since we may have failed
	 * to write to the meta data, and may become wrong anytime because of
	 * local io-error for some other request, which would lead to us
	 * "detaching" the local disk.
	 *
	 * 'remote' may become wrong any time because the network could fail.
	 *
	 * This is a harmless race condition, though, since it is handled
	 * correctly at the appropriate places; so it just defers the failure
	 * of the respective operation.
	 */

	/* mark them early for readability.
	 * this just sets some state flags. */
	if (remote)
		_req_mod(req, TO_BE_SENT);
	if (local)
		_req_mod(req, TO_BE_SUBMITTED);

	list_add_tail(&req->tl_requests, &mdev->tconn->newest_tle->requests);

	/* NOTE remote first: to get the concurrent write detection right,
	 * we must register the request before start of local IO.  */
	if (remote) {
		/* either WRITE and C_CONNECTED,
		 * or READ, and no local disk,
		 * or READ, but not in sync.
		 */
		_req_mod(req, (rw == WRITE)
				? QUEUE_FOR_NET_WRITE
				: QUEUE_FOR_NET_READ);
	}
	if (send_oos && drbd_set_out_of_sync(mdev, sector, size))
		_req_mod(req, QUEUE_FOR_SEND_OOS);

	if (remote &&
	    mdev->tconn->net_conf->on_congestion != OC_BLOCK && mdev->tconn->agreed_pro_version >= 96) {
		int congested = 0;

		if (mdev->tconn->net_conf->cong_fill &&
		    atomic_read(&mdev->ap_in_flight) >= mdev->tconn->net_conf->cong_fill) {
			dev_info(DEV, "Congestion-fill threshold reached\n");
			congested = 1;
		}

		if (mdev->act_log->used >= mdev->tconn->net_conf->cong_extents) {
			dev_info(DEV, "Congestion-extents threshold reached\n");
			congested = 1;
		}

		if (congested) {
			queue_barrier(mdev); /* last barrier, after mirrored writes */

			if (mdev->tconn->net_conf->on_congestion == OC_PULL_AHEAD)
				_drbd_set_state(_NS(mdev, conn, C_AHEAD), 0, NULL);
			else  /*mdev->tconn->net_conf->on_congestion == OC_DISCONNECT */
				_drbd_set_state(_NS(mdev, conn, C_DISCONNECTING), 0, NULL);
		}
	}

	spin_unlock_irq(&mdev->tconn->req_lock);
	kfree(b); /* if someone else has beaten us to it... */

	if (local) {
		req->private_bio->bi_bdev = mdev->ldev->backing_bdev;

		/* State may have changed since we grabbed our reference on the
		 * mdev->ldev member. Double check, and short-circuit to endio.
		 * In case the last activity log transaction failed to get on
		 * stable storage, and this is a WRITE, we may not even submit
		 * this bio. */
		if (get_ldev(mdev)) {
			if (drbd_insert_fault(mdev,   rw == WRITE ? DRBD_FAULT_DT_WR
						    : rw == READ  ? DRBD_FAULT_DT_RD
						    :               DRBD_FAULT_DT_RA))
				bio_endio(req->private_bio, -EIO);
			else
				generic_make_request(req->private_bio);
			put_ldev(mdev);
		} else
			bio_endio(req->private_bio, -EIO);
	}

	return 0;

fail_free_complete:
	if (req->rq_state & RQ_IN_ACT_LOG)
		drbd_al_complete_io(mdev, sector);
fail_and_free_req:
	if (local) {
		bio_put(req->private_bio);
		req->private_bio = NULL;
		put_ldev(mdev);
	}
	if (!ret)
		bio_endio(bio, err);

	drbd_req_free(req);
	dec_ap_bio(mdev);
	kfree(b);

	return ret;
}

int drbd_make_request(struct request_queue *q, struct bio *bio)
{
	unsigned int s_enr, e_enr;
	struct drbd_conf *mdev = (struct drbd_conf *) q->queuedata;
	unsigned long start_time;

	start_time = jiffies;

	/*
	 * what we "blindly" assume:
	 */
	D_ASSERT(bio->bi_size > 0);
	D_ASSERT(IS_ALIGNED(bio->bi_size, 512));
	D_ASSERT(bio->bi_idx == 0);

	/* to make some things easier, force alignment of requests within the
	 * granularity of our hash tables */
	s_enr = bio->bi_sector >> HT_SHIFT;
	e_enr = (bio->bi_sector+(bio->bi_size>>9)-1) >> HT_SHIFT;

	if (likely(s_enr == e_enr)) {
		inc_ap_bio(mdev, 1);
		return __drbd_make_request(mdev, bio, start_time);
	}

	/* can this bio be split generically?
	 * Maybe add our own split-arbitrary-bios function. */
	if (bio->bi_vcnt != 1 || bio->bi_idx != 0 || bio->bi_size > DRBD_MAX_BIO_SIZE) {
		/* rather error out here than BUG in bio_split */
		dev_err(DEV, "bio would need to, but cannot, be split: "
		    "(vcnt=%u,idx=%u,size=%u,sector=%llu)\n",
		    bio->bi_vcnt, bio->bi_idx, bio->bi_size,
		    (unsigned long long)bio->bi_sector);
		bio_endio(bio, -EINVAL);
	} else {
		/* This bio crosses some boundary, so we have to split it. */
		struct bio_pair *bp;
		/* works for the "do not cross hash slot boundaries" case
		 * e.g. sector 262269, size 4096
		 * s_enr = 262269 >> 6 = 4097
		 * e_enr = (262269+8-1) >> 6 = 4098
		 * HT_SHIFT = 6
		 * sps = 64, mask = 63
		 * first_sectors = 64 - (262269 & 63) = 3
		 */
		const sector_t sect = bio->bi_sector;
		const int sps = 1 << HT_SHIFT; /* sectors per slot */
		const int mask = sps - 1;
		const sector_t first_sectors = sps - (sect & mask);
		bp = bio_split(bio, first_sectors);

		/* we need to get a "reference count" (ap_bio_cnt)
		 * to avoid races with the disconnect/reconnect/suspend code.
		 * In case we need to split the bio here, we need to get three references
		 * atomically, otherwise we might deadlock when trying to submit the
		 * second one! */
		inc_ap_bio(mdev, 3);

		D_ASSERT(e_enr == s_enr + 1);

		while (__drbd_make_request(mdev, &bp->bio1, start_time))
			inc_ap_bio(mdev, 1);

		while (__drbd_make_request(mdev, &bp->bio2, start_time))
			inc_ap_bio(mdev, 1);

		dec_ap_bio(mdev);

		bio_pair_release(bp);
	}
	return 0;
}

/* This is called by bio_add_page().  With this function we reduce
 * the number of BIOs that span over multiple DRBD_MAX_BIO_SIZEs
 * units (was AL_EXTENTs).
 *
 * we do the calculation within the lower 32bit of the byte offsets,
 * since we don't care for actual offset, but only check whether it
 * would cross "activity log extent" boundaries.
 *
 * As long as the BIO is empty we have to allow at least one bvec,
 * regardless of size and offset.  so the resulting bio may still
 * cross extent boundaries.  those are dealt with (bio_split) in
 * drbd_make_request.
 */
int drbd_merge_bvec(struct request_queue *q, struct bvec_merge_data *bvm, struct bio_vec *bvec)
{
	struct drbd_conf *mdev = (struct drbd_conf *) q->queuedata;
	unsigned int bio_offset =
		(unsigned int)bvm->bi_sector << 9; /* 32 bit */
	unsigned int bio_size = bvm->bi_size;
	int limit, backing_limit;

	limit = DRBD_MAX_BIO_SIZE
	      - ((bio_offset & (DRBD_MAX_BIO_SIZE-1)) + bio_size);
	if (limit < 0)
		limit = 0;
	if (bio_size == 0) {
		if (limit <= bvec->bv_len)
			limit = bvec->bv_len;
	} else if (limit && get_ldev(mdev)) {
		struct request_queue * const b =
			mdev->ldev->backing_bdev->bd_disk->queue;
		if (b->merge_bvec_fn) {
			backing_limit = b->merge_bvec_fn(b, bvm, bvec);
			limit = min(limit, backing_limit);
		}
		put_ldev(mdev);
	}
	return limit;
}

void request_timer_fn(unsigned long data)
{
	struct drbd_conf *mdev = (struct drbd_conf *) data;
	struct drbd_request *req; /* oldest request */
	struct list_head *le;
	unsigned long et = 0; /* effective timeout = ko_count * timeout */

	if (get_net_conf(mdev->tconn)) {
		et = mdev->tconn->net_conf->timeout*HZ/10 * mdev->tconn->net_conf->ko_count;
		put_net_conf(mdev->tconn);
	}
	if (!et || mdev->state.conn < C_WF_REPORT_PARAMS)
		return; /* Recurring timer stopped */

	spin_lock_irq(&mdev->tconn->req_lock);
	le = &mdev->tconn->oldest_tle->requests;
	if (list_empty(le)) {
		spin_unlock_irq(&mdev->tconn->req_lock);
		mod_timer(&mdev->request_timer, jiffies + et);
		return;
	}

	le = le->prev;
	req = list_entry(le, struct drbd_request, tl_requests);
	if (time_is_before_eq_jiffies(req->start_time + et)) {
		if (req->rq_state & RQ_NET_PENDING) {
			dev_warn(DEV, "Remote failed to finish a request within ko-count * timeout\n");
			_drbd_set_state(_NS(mdev, conn, C_TIMEOUT), CS_VERBOSE, NULL);
		} else {
			dev_warn(DEV, "Local backing block device frozen?\n");
			mod_timer(&mdev->request_timer, jiffies + et);
		}
	} else {
		mod_timer(&mdev->request_timer, req->start_time + et);
	}

	spin_unlock_irq(&mdev->tconn->req_lock);
}
