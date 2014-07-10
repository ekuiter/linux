#define pr_fmt(fmt) "drbd debugfs: " fmt
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/jiffies.h>
#include <linux/list.h>

#include "drbd_int.h"
#include "drbd_req.h"
#include "drbd_debugfs.h"

static struct dentry *drbd_debugfs_root;
static struct dentry *drbd_debugfs_resources;
static struct dentry *drbd_debugfs_minors;

static void seq_print_age_or_dash(struct seq_file *m, bool valid, unsigned long dt)
{
	if (valid)
		seq_printf(m, "\t%d", jiffies_to_msecs(dt));
	else
		seq_printf(m, "\t-");
}

static void __seq_print_rq_state_bit(struct seq_file *m,
	bool is_set, char *sep, const char *set_name, const char *unset_name)
{
	if (is_set && set_name) {
		seq_putc(m, *sep);
		seq_puts(m, set_name);
		*sep = '|';
	} else if (!is_set && unset_name) {
		seq_putc(m, *sep);
		seq_puts(m, unset_name);
		*sep = '|';
	}
}

static void seq_print_rq_state_bit(struct seq_file *m,
	bool is_set, char *sep, const char *set_name)
{
	__seq_print_rq_state_bit(m, is_set, sep, set_name, NULL);
}

/* pretty print enum drbd_req_state_bits req->rq_state */
static void seq_print_request_state(struct seq_file *m, struct drbd_request *req)
{
	unsigned int s = req->rq_state;
	char sep = ' ';
	seq_printf(m, "\t0x%08x", s);
	seq_printf(m, "\tmaster: %s", req->master_bio ? "pending" : "completed");

	/* RQ_WRITE ignored, already reported */
	seq_puts(m, "\tlocal:");
	seq_print_rq_state_bit(m, s & RQ_IN_ACT_LOG, &sep, "in-AL");
	seq_print_rq_state_bit(m, s & RQ_POSTPONED, &sep, "postponed");
	seq_print_rq_state_bit(m, s & RQ_COMPLETION_SUSP, &sep, "suspended");
	sep = ' ';
	seq_print_rq_state_bit(m, s & RQ_LOCAL_PENDING, &sep, "pending");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_COMPLETED, &sep, "completed");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_ABORTED, &sep, "aborted");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_OK, &sep, "ok");
	if (sep == ' ')
		seq_puts(m, " -");

	/* for_each_connection ... */
	seq_printf(m, "\tnet:");
	sep = ' ';
	seq_print_rq_state_bit(m, s & RQ_NET_PENDING, &sep, "pending");
	seq_print_rq_state_bit(m, s & RQ_NET_QUEUED, &sep, "queued");
	seq_print_rq_state_bit(m, s & RQ_NET_SENT, &sep, "sent");
	seq_print_rq_state_bit(m, s & RQ_NET_DONE, &sep, "done");
	seq_print_rq_state_bit(m, s & RQ_NET_SIS, &sep, "sis");
	seq_print_rq_state_bit(m, s & RQ_NET_OK, &sep, "ok");
	if (sep == ' ')
		seq_puts(m, " -");

	seq_printf(m, " :");
	sep = ' ';
	seq_print_rq_state_bit(m, s & RQ_EXP_RECEIVE_ACK, &sep, "B");
	seq_print_rq_state_bit(m, s & RQ_EXP_WRITE_ACK, &sep, "C");
	seq_print_rq_state_bit(m, s & RQ_EXP_BARR_ACK, &sep, "barr");
	if (sep == ' ')
		seq_puts(m, " -");
	seq_printf(m, "\n");
}

static void seq_print_one_request(struct seq_file *m, struct drbd_request *req, unsigned long now)
{
	/* change anything here, fixup header below! */
	unsigned int s = req->rq_state;

#define RQ_HDR_1 "epoch\tsector\tsize\trw"
	seq_printf(m, "0x%x\t%llu\t%u\t%s",
		req->epoch,
		(unsigned long long)req->i.sector, req->i.size >> 9,
		(s & RQ_WRITE) ? "W" : "R");

#define RQ_HDR_2 "\tstart\tin AL\tsubmit"
	seq_printf(m, "\t%d", jiffies_to_msecs(now - req->start_jif));
	seq_print_age_or_dash(m, s & RQ_IN_ACT_LOG, now - req->in_actlog_jif);
	seq_print_age_or_dash(m, s & RQ_LOCAL_PENDING, now - req->pre_submit_jif);

#define RQ_HDR_3 "\tsent\tacked\tdone"
	seq_print_age_or_dash(m, s & RQ_NET_SENT, now - req->pre_send_jif);
	seq_print_age_or_dash(m, (s & RQ_NET_SENT) && !(s & RQ_NET_PENDING), now - req->acked_jif);
	seq_print_age_or_dash(m, s & RQ_NET_DONE, now - req->net_done_jif);

#define RQ_HDR_4 "\tstate\n"
	seq_print_request_state(m, req);
}
#define RQ_HDR RQ_HDR_1 RQ_HDR_2 RQ_HDR_3 RQ_HDR_4

static void seq_print_minor_vnr_req(struct seq_file *m, struct drbd_request *req, unsigned long now)
{
	seq_printf(m, "%u\t%u\t", req->device->minor, req->device->vnr);
	seq_print_one_request(m, req, now);
}

static void seq_print_resource_pending_meta_io(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\tstart\tsubmit\tintent\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		struct drbd_md_io tmp;
		/* In theory this is racy,
		 * in the sense that there could have been a
		 * drbd_md_put_buffer(); drbd_md_get_buffer();
		 * between accessing these members here.  */
		tmp = device->md_io;
		if (atomic_read(&tmp.in_use)) {
			seq_printf(m, "%u\t%u\t%d\t",
				device->minor, device->vnr,
				jiffies_to_msecs(now - tmp.start_jif));
			if (time_before(tmp.submit_jif, tmp.start_jif))
				seq_puts(m, "-\t");
			else
				seq_printf(m, "%d\t", jiffies_to_msecs(now - tmp.submit_jif));
			seq_printf(m, "%s\n", tmp.current_use);
		}
	}
	rcu_read_unlock();
}

static void seq_print_waiting_for_AL(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\tage\t#waiting\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		unsigned long jif;
		struct drbd_request *req;
		int n = atomic_read(&device->ap_actlog_cnt);
		if (n) {
			spin_lock_irq(&device->resource->req_lock);
			req = list_first_entry_or_null(&device->pending_master_completion[1],
				struct drbd_request, req_pending_master_completion);
			/* if the oldest request does not wait for the activity log
			 * it is not interesting for us here */
			if (req && !(req->rq_state & RQ_IN_ACT_LOG))
				jif = req->start_jif;
			else
				req = NULL;
			spin_unlock_irq(&device->resource->req_lock);
		}
		if (n) {
			seq_printf(m, "%u\t%u\t", device->minor, device->vnr);
			if (req)
				seq_printf(m, "%u\t", jiffies_to_msecs(now - jif));
			else
				seq_puts(m, "-\t");
			seq_printf(m, "%u\n", n);
		}
	}
	rcu_read_unlock();
}

static void seq_print_device_bitmap_io(struct seq_file *m, struct drbd_device *device, unsigned long now)
{
	struct drbd_bm_aio_ctx *ctx;
	unsigned long start_jif;
	unsigned int in_flight;
	unsigned int flags;
	spin_lock_irq(&device->resource->req_lock);
	ctx = list_first_entry_or_null(&device->pending_bitmap_io, struct drbd_bm_aio_ctx, list);
	if (ctx && ctx->done)
		ctx = NULL;
	if (ctx) {
		start_jif = ctx->start_jif;
		in_flight = atomic_read(&ctx->in_flight);
		flags = ctx->flags;
	}
	spin_unlock_irq(&device->resource->req_lock);
	if (ctx) {
		seq_printf(m, "%u\t%u\t%c\t%u\t%u\n",
			device->minor, device->vnr,
			(flags & BM_AIO_READ) ? 'R' : 'W',
			jiffies_to_msecs(now - start_jif),
			in_flight);
	}
}

static void seq_print_resource_pending_bitmap_io(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\trw\tage\t#in-flight\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		seq_print_device_bitmap_io(m, device, now);
	}
	rcu_read_unlock();
}

/* pretty print enum peer_req->flags */
static void seq_print_peer_request_flags(struct seq_file *m, struct drbd_peer_request *peer_req)
{
	unsigned long f = peer_req->flags;
	char sep = ' ';

	__seq_print_rq_state_bit(m, f & EE_SUBMITTED, &sep, "submitted", "preparing");
	__seq_print_rq_state_bit(m, f & EE_APPLICATION, &sep, "application", "internal");
	seq_print_rq_state_bit(m, f & EE_CALL_AL_COMPLETE_IO, &sep, "in-AL");
	seq_print_rq_state_bit(m, f & EE_SEND_WRITE_ACK, &sep, "C");
	seq_print_rq_state_bit(m, f & EE_MAY_SET_IN_SYNC, &sep, "set-in-sync");

	if (f & EE_IS_TRIM) {
		seq_putc(m, sep);
		sep = '|';
		if (f & EE_IS_TRIM_USE_ZEROOUT)
			seq_puts(m, "zero-out");
		else
			seq_puts(m, "trim");
	}
	seq_putc(m, '\n');
}

static void seq_print_peer_request(struct seq_file *m,
	struct drbd_device *device, struct list_head *lh,
	unsigned long now)
{
	bool reported_preparing = false;
	struct drbd_peer_request *peer_req;
	list_for_each_entry(peer_req, lh, w.list) {
		if (reported_preparing && !(peer_req->flags & EE_SUBMITTED))
			continue;

		if (device)
			seq_printf(m, "%u\t%u\t", device->minor, device->vnr);

		seq_printf(m, "%llu\t%u\t%c\t%u\t",
			(unsigned long long)peer_req->i.sector, peer_req->i.size >> 9,
			(peer_req->flags & EE_WRITE) ? 'W' : 'R',
			jiffies_to_msecs(now - peer_req->submit_jif));
		seq_print_peer_request_flags(m, peer_req);
		if (peer_req->flags & EE_SUBMITTED)
			break;
		else
			reported_preparing = true;
	}
}

static void seq_print_device_peer_requests(struct seq_file *m,
	struct drbd_device *device, unsigned long now)
{
	seq_puts(m, "minor\tvnr\tsector\tsize\trw\tage\tflags\n");
	spin_lock_irq(&device->resource->req_lock);
	seq_print_peer_request(m, device, &device->active_ee, now);
	seq_print_peer_request(m, device, &device->read_ee, now);
	seq_print_peer_request(m, device, &device->sync_ee, now);
	spin_unlock_irq(&device->resource->req_lock);
	if (test_bit(FLUSH_PENDING, &device->flags)) {
		seq_printf(m, "%u\t%u\t-\t-\tF\t%u\tflush\n",
			device->minor, device->vnr,
			jiffies_to_msecs(now - device->flush_jif));
	}
}

static void seq_print_resource_pending_peer_requests(struct seq_file *m,
	struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		seq_print_device_peer_requests(m, device, now);
	}
	rcu_read_unlock();
}

static void seq_print_resource_transfer_log_summary(struct seq_file *m,
	struct drbd_resource *resource,
	struct drbd_connection *connection,
	unsigned long now)
{
	struct drbd_request *req;
	unsigned int count = 0;
	unsigned int show_state = 0;

	seq_puts(m, "n\tdevice\tvnr\t" RQ_HDR);
	spin_lock_irq(&resource->req_lock);
	list_for_each_entry(req, &connection->transfer_log, tl_requests) {
		unsigned int tmp = 0;
		unsigned int s;
		++count;

		/* don't disable irq "forever" */
		if (!(count & 0x1ff)) {
			struct drbd_request *req_next;
			kref_get(&req->kref);
			spin_unlock_irq(&resource->req_lock);
			cond_resched();
			spin_lock_irq(&resource->req_lock);
			req_next = list_next_entry(req, tl_requests);
			if (kref_put(&req->kref, drbd_req_destroy))
				req = req_next;
			if (&req->tl_requests == &connection->transfer_log)
				break;
		}

		s = req->rq_state;

		/* This is meant to summarize timing issues, to be able to tell
		 * local disk problems from network problems.
		 * Skip requests, if we have shown an even older request with
		 * similar aspects already.  */
		if (req->master_bio == NULL)
			tmp |= 1;
		if ((s & RQ_LOCAL_MASK) && (s & RQ_LOCAL_PENDING))
			tmp |= 2;
		if (s & RQ_NET_MASK) {
			if (!(s & RQ_NET_SENT))
				tmp |= 4;
			if (s & RQ_NET_PENDING)
				tmp |= 8;
			if (!(s & RQ_NET_DONE))
				tmp |= 16;
		}
		if ((tmp & show_state) == tmp)
			continue;
		show_state |= tmp;
		seq_printf(m, "%u\t", count);
		seq_print_minor_vnr_req(m, req, now);
		if (show_state == 0x1f)
			break;
	}
	spin_unlock_irq(&resource->req_lock);
}

/* TODO: transfer_log and friends should be moved to resource */
static int in_flight_summary_show(struct seq_file *m, void *pos)
{
	struct drbd_resource *resource = m->private;
	struct drbd_connection *connection;
	unsigned long jif = jiffies;

	connection = first_connection(resource);
	/* This does not happen, actually.
	 * But be robust and prepare for future code changes. */
	if (!connection || !kref_get_unless_zero(&connection->kref))
		return -ESTALE;

	seq_puts(m, "oldest bitmap IO\n");
	seq_print_resource_pending_bitmap_io(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "meta data IO\n");
	seq_print_resource_pending_meta_io(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "socket buffer stats\n");
	/* for each connection ... once we have more than one */
	rcu_read_lock();
	if (connection->data.socket) {
		/* open coded SIOCINQ, the "relevant" part */
		struct tcp_sock *tp = tcp_sk(connection->data.socket->sk);
		int answ = tp->rcv_nxt - tp->copied_seq;
		seq_printf(m, "unread receive buffer: %u Byte\n", answ);
		/* open coded SIOCOUTQ, the "relevant" part */
		answ = tp->write_seq - tp->snd_una;
		seq_printf(m, "unacked send buffer: %u Byte\n", answ);
	}
	rcu_read_unlock();
	seq_putc(m, '\n');

	seq_puts(m, "oldest peer requests\n");
	seq_print_resource_pending_peer_requests(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "application requests waiting for activity log\n");
	seq_print_waiting_for_AL(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "oldest application requests\n");
	seq_print_resource_transfer_log_summary(m, resource, connection, jif);
	seq_putc(m, '\n');

	jif = jiffies - jif;
	if (jif)
		seq_printf(m, "generated in %d ms\n", jiffies_to_msecs(jif));
	kref_put(&connection->kref, drbd_destroy_connection);
	return 0;
}

/* simple_positive(file->f_dentry) respectively debugfs_positive(),
 * but neither is "reachable" from here.
 * So we have our own inline version of it above.  :-( */
static inline int debugfs_positive(struct dentry *dentry)
{
        return dentry->d_inode && !d_unhashed(dentry);
}

/* make sure at *open* time that the respective object won't go away. */
static int drbd_single_open(struct file *file, int (*show)(struct seq_file *, void *),
		                void *data, struct kref *kref,
				void (*release)(struct kref *))
{
	struct dentry *parent;
	int ret = -ESTALE;

	/* Are we still linked,
	 * or has debugfs_remove() already been called? */
	parent = file->f_dentry->d_parent;
	/* not sure if this can happen: */
	if (!parent || !parent->d_inode)
		goto out;
	/* serialize with d_delete() */
	mutex_lock(&parent->d_inode->i_mutex);
	if (!debugfs_positive(file->f_dentry))
		goto out_unlock;
	/* Make sure the object is still alive */
	if (kref_get_unless_zero(kref))
		ret = 0;
out_unlock:
	mutex_unlock(&parent->d_inode->i_mutex);
	if (!ret) {
		ret = single_open(file, show, data);
		if (ret)
			kref_put(kref, release);
	}
out:
	return ret;
}

static int in_flight_summary_open(struct inode *inode, struct file *file)
{
	struct drbd_resource *resource = inode->i_private;
	return drbd_single_open(file, in_flight_summary_show, resource,
				&resource->kref, drbd_destroy_resource);
}

static int in_flight_summary_release(struct inode *inode, struct file *file)
{
	struct drbd_resource *resource = inode->i_private;
	kref_put(&resource->kref, drbd_destroy_resource);
	return single_release(inode, file);
}

static const struct file_operations in_flight_summary_fops = {
	.owner		= THIS_MODULE,
	.open		= in_flight_summary_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= in_flight_summary_release,
};

void drbd_debugfs_resource_add(struct drbd_resource *resource)
{
	struct dentry *dentry;
	if (!drbd_debugfs_resources)
		return;

	dentry = debugfs_create_dir(resource->name, drbd_debugfs_resources);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res = dentry;

	dentry = debugfs_create_dir("volumes", resource->debugfs_res);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_volumes = dentry;

	dentry = debugfs_create_dir("connections", resource->debugfs_res);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_connections = dentry;

	dentry = debugfs_create_file("in_flight_summary", S_IRUSR|S_IRGRP,
			resource->debugfs_res, resource,
			&in_flight_summary_fops);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_in_flight_summary = dentry;
	return;

fail:
	drbd_debugfs_resource_cleanup(resource);
	drbd_err(resource, "failed to create debugfs dentry\n");
}

static void drbd_debugfs_remove(struct dentry **dp)
{
	debugfs_remove(*dp);
	*dp = NULL;
}

void drbd_debugfs_resource_cleanup(struct drbd_resource *resource)
{
	/* it is ok to call debugfs_remove(NULL) */
	drbd_debugfs_remove(&resource->debugfs_res_in_flight_summary);
	drbd_debugfs_remove(&resource->debugfs_res_connections);
	drbd_debugfs_remove(&resource->debugfs_res_volumes);
	drbd_debugfs_remove(&resource->debugfs_res);
}

void drbd_debugfs_connection_add(struct drbd_connection *connection)
{
	struct dentry *conns_dir = connection->resource->debugfs_res_connections;
	struct dentry *dentry;
	if (!conns_dir)
		return;

	/* Once we enable mutliple peers,
	 * these connections will have descriptive names.
	 * For now, it is just the one connection to the (only) "peer". */
	dentry = debugfs_create_dir("peer", conns_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	connection->debugfs_conn = dentry;
	return;

fail:
	drbd_debugfs_connection_cleanup(connection);
	drbd_err(connection, "failed to create debugfs dentry\n");
}

void drbd_debugfs_connection_cleanup(struct drbd_connection *connection)
{
	drbd_debugfs_remove(&connection->debugfs_conn_callback_history);
	drbd_debugfs_remove(&connection->debugfs_conn_oldest_requests);
	drbd_debugfs_remove(&connection->debugfs_conn);
}

void drbd_debugfs_device_add(struct drbd_device *device)
{
	struct dentry *vols_dir = device->resource->debugfs_res_volumes;
	char minor_buf[8]; /* MINORMASK, MINORBITS == 20; */
	char vnr_buf[8];   /* volume number vnr is even 16 bit only; */
	char *slink_name = NULL;

	struct dentry *dentry;
	if (!vols_dir || !drbd_debugfs_minors)
		return;

	snprintf(vnr_buf, sizeof(vnr_buf), "%u", device->vnr);
	dentry = debugfs_create_dir(vnr_buf, vols_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	device->debugfs_vol = dentry;

	snprintf(minor_buf, sizeof(minor_buf), "%u", device->minor);
	slink_name = kasprintf(GFP_KERNEL, "../resources/%s/volumes/%u",
			device->resource->name, device->vnr);
	if (!slink_name)
		goto fail;
	dentry = debugfs_create_symlink(minor_buf, drbd_debugfs_minors, slink_name);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	device->debugfs_minor = dentry;
	kfree(slink_name);

fail:
	drbd_debugfs_device_cleanup(device);
	drbd_err(device, "failed to create debugfs entries\n");
}

void drbd_debugfs_device_cleanup(struct drbd_device *device)
{
	drbd_debugfs_remove(&device->debugfs_minor);
	drbd_debugfs_remove(&device->debugfs_vol_oldest_requests);
	drbd_debugfs_remove(&device->debugfs_vol_act_log_extents);
	drbd_debugfs_remove(&device->debugfs_vol_resync_extents);
	drbd_debugfs_remove(&device->debugfs_vol_data_gen_id);
	drbd_debugfs_remove(&device->debugfs_vol);
}

void drbd_debugfs_peer_device_add(struct drbd_peer_device *peer_device)
{
	struct dentry *conn_dir = peer_device->connection->debugfs_conn;
	struct dentry *dentry;
	char vnr_buf[8];

	if (!conn_dir)
		return;

	snprintf(vnr_buf, sizeof(vnr_buf), "%u", peer_device->device->vnr);
	dentry = debugfs_create_dir(vnr_buf, conn_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	peer_device->debugfs_peer_dev = dentry;
	return;

fail:
	drbd_debugfs_peer_device_cleanup(peer_device);
	drbd_err(peer_device, "failed to create debugfs entries\n");
}

void drbd_debugfs_peer_device_cleanup(struct drbd_peer_device *peer_device)
{
	drbd_debugfs_remove(&peer_device->debugfs_peer_dev);
}

/* not __exit, may be indirectly called
 * from the module-load-failure path as well. */
void drbd_debugfs_cleanup(void)
{
	drbd_debugfs_remove(&drbd_debugfs_resources);
	drbd_debugfs_remove(&drbd_debugfs_minors);
	drbd_debugfs_remove(&drbd_debugfs_root);
}

int __init drbd_debugfs_init(void)
{
	struct dentry *dentry;

	dentry = debugfs_create_dir("drbd", NULL);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_root = dentry;

	dentry = debugfs_create_dir("resources", drbd_debugfs_root);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_resources = dentry;

	dentry = debugfs_create_dir("minors", drbd_debugfs_root);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_minors = dentry;
	return 0;

fail:
	drbd_debugfs_cleanup();
	if (dentry)
		return PTR_ERR(dentry);
	else
		return -EINVAL;
}
