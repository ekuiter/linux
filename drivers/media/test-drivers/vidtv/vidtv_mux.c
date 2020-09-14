// SPDX-License-Identifier: GPL-2.0
/*
 * Vidtv serves as a reference DVB driver and helps validate the existing APIs
 * in the media subsystem. It can also aid developers working on userspace
 * applications.
 *
 * This file contains the multiplexer logic for TS packets from different
 * elementary streams
 *
 * Loosely based on libavcodec/mpegtsenc.c
 *
 * Copyright (C) 2020 Daniel W. S. Almeida
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/dev_printk.h>
#include <linux/ratelimit.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include "vidtv_mux.h"
#include "vidtv_ts.h"
#include "vidtv_pes.h"
#include "vidtv_encoder.h"
#include "vidtv_channel.h"
#include "vidtv_common.h"
#include "vidtv_psi.h"

static struct vidtv_mux_pid_ctx
*vidtv_mux_get_pid_ctx(struct vidtv_mux *m, u16 pid)
{
	struct vidtv_mux_pid_ctx *ctx;

	hash_for_each_possible(m->pid_ctx, ctx, h, pid)
		if (ctx->pid == pid)
			return ctx;
	return NULL;
}

static struct vidtv_mux_pid_ctx
*vidtv_mux_create_pid_ctx_once(struct vidtv_mux *m, u16 pid)
{
	struct vidtv_mux_pid_ctx *ctx;

	ctx = vidtv_mux_get_pid_ctx(m, pid);

	if (ctx)
		goto end;

	ctx      = kzalloc(sizeof(*ctx), GFP_KERNEL);
	ctx->pid = pid;
	ctx->cc  = 0;
	hash_add(m->pid_ctx, &ctx->h, pid);

end:
	return ctx;
}

static void vidtv_mux_pid_ctx_init(struct vidtv_mux *m)
{
	struct vidtv_psi_table_pat_program *p = m->si.pat->program;
	u16 pid;

	hash_init(m->pid_ctx);
	/* push the pcr pid ctx */
	vidtv_mux_create_pid_ctx_once(m, m->pcr_pid);
	/* push the null packet pid ctx */
	vidtv_mux_create_pid_ctx_once(m, TS_NULL_PACKET_PID);
	/* push the PAT pid ctx */
	vidtv_mux_create_pid_ctx_once(m, VIDTV_PAT_PID);
	/* push the SDT pid ctx */
	vidtv_mux_create_pid_ctx_once(m, VIDTV_SDT_PID);

	/* add a ctx for all PMT sections */
	while (p) {
		pid = vidtv_psi_get_pat_program_pid(p);
		vidtv_mux_create_pid_ctx_once(m, pid);
		p = p->next;
	}
}

static void vidtv_mux_pid_ctx_destroy(struct vidtv_mux *m)
{
	int bkt;
	struct vidtv_mux_pid_ctx *ctx;
	struct hlist_node *tmp;

	hash_for_each_safe(m->pid_ctx, bkt, tmp, ctx, h) {
		hash_del(&ctx->h);
		kfree(ctx);
	}
}

static void vidtv_mux_update_clk(struct vidtv_mux *m)
{
	/* call this at every thread iteration */
	u64 elapsed_time;

	/* this will not hold a value yet if we have just started */
	m->timing.past_jiffies = m->timing.current_jiffies ?
				 m->timing.current_jiffies :
				 get_jiffies_64();

	m->timing.current_jiffies = get_jiffies_64();

	elapsed_time = jiffies_to_usecs(m->timing.current_jiffies -
					m->timing.past_jiffies);

	/* update the 27Mhz clock proportionally to the elapsed time */
	m->timing.clk += (CLOCK_UNIT_27MHZ / USEC_PER_SEC) * elapsed_time;
}

static u32 vidtv_mux_push_si(struct vidtv_mux *m)
{
	u32 initial_offset = m->mux_buf_offset;

	struct vidtv_mux_pid_ctx *pat_ctx;
	struct vidtv_mux_pid_ctx *pmt_ctx;
	struct vidtv_mux_pid_ctx *sdt_ctx;

	struct vidtv_psi_pat_write_args pat_args = {};
	struct vidtv_psi_pmt_write_args pmt_args = {};
	struct vidtv_psi_sdt_write_args sdt_args = {};

	u32 nbytes; /* the number of bytes written by this function */
	u16 pmt_pid;
	u32 i;

	pat_ctx = vidtv_mux_get_pid_ctx(m, VIDTV_PAT_PID);
	sdt_ctx = vidtv_mux_get_pid_ctx(m, VIDTV_SDT_PID);

	pat_args.buf                = m->mux_buf;
	pat_args.offset             = m->mux_buf_offset;
	pat_args.pat                = m->si.pat;
	pat_args.buf_sz             = m->mux_buf_sz;
	pat_args.continuity_counter = &pat_ctx->cc;

	m->mux_buf_offset += vidtv_psi_pat_write_into(pat_args);

	for (i = 0; i < m->si.pat->programs; ++i) {
		pmt_pid = vidtv_psi_pmt_get_pid(m->si.pmt_secs[i],
						m->si.pat);

		if (pmt_pid > TS_LAST_VALID_PID) {
			dev_warn_ratelimited(m->dev,
					     "PID: %d not found\n", pmt_pid);
			continue;
		}

		pmt_ctx = vidtv_mux_get_pid_ctx(m, pmt_pid);

		pmt_args.buf                = m->mux_buf;
		pmt_args.offset             = m->mux_buf_offset;
		pmt_args.pmt                = m->si.pmt_secs[i];
		pmt_args.pid                = pmt_pid;
		pmt_args.buf_sz             = m->mux_buf_sz;
		pmt_args.continuity_counter = &pmt_ctx->cc;
		pmt_args.pcr_pid            = m->pcr_pid;

		/* write each section into buffer */
		m->mux_buf_offset += vidtv_psi_pmt_write_into(pmt_args);
	}

	sdt_args.buf                = m->mux_buf;
	sdt_args.offset             = m->mux_buf_offset;
	sdt_args.sdt                = m->si.sdt;
	sdt_args.buf_sz             = m->mux_buf_sz;
	sdt_args.continuity_counter = &sdt_ctx->cc;

	m->mux_buf_offset += vidtv_psi_sdt_write_into(sdt_args);

	nbytes = m->mux_buf_offset - initial_offset;

	m->num_streamed_si++;

	return nbytes;
}

static u32 vidtv_mux_push_pcr(struct vidtv_mux *m)
{
	struct pcr_write_args args = {};
	struct vidtv_mux_pid_ctx *ctx;
	u32 nbytes = 0;

	ctx                     = vidtv_mux_get_pid_ctx(m, m->pcr_pid);
	args.dest_buf           = m->mux_buf;
	args.pid                = m->pcr_pid;
	args.buf_sz             = m->mux_buf_sz;
	args.continuity_counter = &ctx->cc;

	/* the 27Mhz clock will feed both parts of the PCR bitfield */
	args.pcr = m->timing.clk;

	nbytes += vidtv_ts_pcr_write_into(args);
	m->mux_buf_offset += nbytes;

	m->num_streamed_pcr++;

	return nbytes;
}

static bool vidtv_mux_should_push_pcr(struct vidtv_mux *m)
{
	u64 next_pcr_at;

	if (m->num_streamed_pcr == 0)
		return true;

	next_pcr_at = m->timing.start_jiffies +
		      usecs_to_jiffies(m->num_streamed_pcr *
				       m->timing.pcr_period_usecs);

	return time_after64(m->timing.current_jiffies, next_pcr_at);
}

static bool vidtv_mux_should_push_si(struct vidtv_mux *m)
{
	u64 next_si_at;

	if (m->num_streamed_si == 0)
		return true;

	next_si_at = m->timing.start_jiffies +
		     usecs_to_jiffies(m->num_streamed_si *
				      m->timing.si_period_usecs);

	return time_after64(m->timing.current_jiffies, next_si_at);
}

static u32 vidtv_mux_packetize_access_units(struct vidtv_mux *m,
					    struct vidtv_encoder *e)
{
	u32 nbytes = 0;

	struct pes_write_args args = {};
	u32 initial_offset = m->mux_buf_offset;
	struct vidtv_access_unit *au = e->access_units;

	u8 *buf = NULL;
	struct vidtv_mux_pid_ctx *pid_ctx = vidtv_mux_create_pid_ctx_once(m,
									  be16_to_cpu(e->es_pid));

	args.dest_buf           = m->mux_buf;
	args.dest_buf_sz        = m->mux_buf_sz;
	args.pid                = be16_to_cpu(e->es_pid);
	args.encoder_id         = e->id;
	args.continuity_counter = &pid_ctx->cc;
	args.stream_id          = be16_to_cpu(e->stream_id);
	args.send_pts           = true;

	while (au) {
		buf                  = e->encoder_buf + au->offset;
		args.from            = buf;
		args.access_unit_len = au->nbytes;
		args.dest_offset     = m->mux_buf_offset;
		args.pts             = au->pts;

		m->mux_buf_offset += vidtv_pes_write_into(args);

		au = au->next;
	}

	/*
	 * clear the encoder state once the ES data has been written to the mux
	 * buffer
	 */
	e->clear(e);

	nbytes = m->mux_buf_offset - initial_offset;
	return nbytes;
}

static u32 vidtv_mux_poll_encoders(struct vidtv_mux *m)
{
	u32 nbytes = 0;
	u32 au_nbytes;
	struct vidtv_channel *cur_chnl = m->channels;
	struct vidtv_encoder *e = NULL;

	u64 elapsed_time_usecs = jiffies_to_usecs(m->timing.current_jiffies -
						  m->timing.past_jiffies);

	elapsed_time_usecs = min_t(u64, elapsed_time_usecs, (u64)VIDTV_MAX_SLEEP_USECS);

	while (cur_chnl) {
		e = cur_chnl->encoders;

		while (e) {
			/* encode for 'elapsed_time_usecs' */
			e->encode(e, elapsed_time_usecs);
			/* get the TS packets into the mux buffer */
			au_nbytes = vidtv_mux_packetize_access_units(m, e);
			nbytes += au_nbytes;
			m->mux_buf_offset += au_nbytes;
			/* grab next encoder */
			e = e->next;
		}

		/* grab the next channel */
		cur_chnl = cur_chnl->next;
	}

	return nbytes;
}

static u32 vidtv_mux_pad_with_nulls(struct vidtv_mux *m, u32 npkts)
{
	struct null_packet_write_args args = {};
	u32 initial_offset = m->mux_buf_offset;
	u32 nbytes; /* the number of bytes written by this function */
	u32 i;
	struct vidtv_mux_pid_ctx *ctx;

	ctx = vidtv_mux_get_pid_ctx(m, TS_NULL_PACKET_PID);

	args.dest_buf           = m->mux_buf;
	args.buf_sz             = m->mux_buf_sz;
	args.continuity_counter = &ctx->cc;
	args.dest_offset        = m->mux_buf_offset;

	for (i = 0; i < npkts; ++i) {
		m->mux_buf_offset += vidtv_ts_null_write_into(args);
		args.dest_offset  = m->mux_buf_offset;
	}

	nbytes = m->mux_buf_offset - initial_offset;

	/* sanity check */
	if (nbytes != npkts * TS_PACKET_LEN)
		dev_err_ratelimited(m->dev, "%d != %d\n",
				    nbytes, npkts * TS_PACKET_LEN);

	return nbytes;
}

static u32 vidtv_mux_check_mux_rate(struct vidtv_mux *m)
{
	/*
	 * attempt to maintain a constant mux rate, padding with null packets
	 * if needed
	 */

	u32 nbytes = 0;  /* the number of bytes written by this function */

	u64 nbytes_expected; /* the number of bytes we should have written */
	u64 nbytes_streamed; /* the number of bytes we actually wrote */
	u32 num_null_pkts; /* number of null packets to bridge the gap */

	u64 elapsed_time_msecs = jiffies_to_usecs(m->timing.current_jiffies -
						  m->timing.past_jiffies);

	elapsed_time_msecs = min(elapsed_time_msecs, (u64)VIDTV_MAX_SLEEP_USECS / 1000);
	nbytes_expected = div64_u64(m->mux_rate_kbytes_sec * 1000, MSEC_PER_SEC);
	nbytes_expected *= elapsed_time_msecs;

	nbytes_streamed = m->mux_buf_offset;

	if (nbytes_streamed < nbytes_expected) {
		/* can't write half a packet: roundup to a 188 multiple */
		nbytes_expected  = roundup(nbytes_expected - nbytes_streamed, TS_PACKET_LEN);
		num_null_pkts    = nbytes_expected / TS_PACKET_LEN;
		nbytes          += vidtv_mux_pad_with_nulls(m, num_null_pkts);
	}

	return nbytes;
}

static void vidtv_mux_clear(struct vidtv_mux *m)
{
	/* clear the packets currently in the mux */
	memset(m->mux_buf, 0, m->mux_buf_sz * sizeof(*m->mux_buf));
	/* point to the beginning of the buffer again */
	m->mux_buf_offset = 0;
}

static void vidtv_mux_tick(struct work_struct *work)
{
	struct vidtv_mux *m = container_of(work,
					   struct vidtv_mux,
					   mpeg_thread);
	struct dtv_frontend_properties *c = &m->fe->dtv_property_cache;
	u32 nbytes;
	u32 npkts;

	while (m->streaming) {
		nbytes = 0;

		vidtv_mux_update_clk(m);

		if (vidtv_mux_should_push_pcr(m))
			nbytes += vidtv_mux_push_pcr(m);

		if (vidtv_mux_should_push_si(m))
			nbytes += vidtv_mux_push_si(m);

		nbytes += vidtv_mux_poll_encoders(m);
		nbytes += vidtv_mux_check_mux_rate(m);

		npkts = nbytes / TS_PACKET_LEN;

		/* if the buffer is not aligned there is a bug somewhere */
		if (nbytes % TS_PACKET_LEN)
			dev_err_ratelimited(m->dev, "Misaligned buffer\n");

		if (m->on_new_packets_available_cb)
			m->on_new_packets_available_cb(m->priv,
						       m->mux_buf,
						       npkts);

		vidtv_mux_clear(m);

		/*
		 * Update bytes and packet counts at DVBv5 stats
		 *
		 * For now, both pre and post bit counts are identical,
		 * but post BER count can be lower than pre BER, if the error
		 * correction logic discards packages.
		 */
		c->pre_bit_count.stat[0].uvalue = nbytes;
		c->post_bit_count.stat[0].uvalue = nbytes;
		c->block_count.stat[0].uvalue += npkts;

		usleep_range(VIDTV_SLEEP_USECS, VIDTV_MAX_SLEEP_USECS);
	}
}

void vidtv_mux_start_thread(struct vidtv_mux *m)
{
	if (m->streaming) {
		dev_warn_ratelimited(m->dev, "Already streaming. Skipping.\n");
		return;
	}

	m->streaming = true;
	m->timing.start_jiffies = get_jiffies_64();
	schedule_work(&m->mpeg_thread);
}

void vidtv_mux_stop_thread(struct vidtv_mux *m)
{
	if (m->streaming) {
		m->streaming = false; /* thread will quit */
		cancel_work_sync(&m->mpeg_thread);
	}
}

struct vidtv_mux *vidtv_mux_init(struct dvb_frontend *fe,
				 struct device *dev,
				 struct vidtv_mux_init_args args)
{
	struct vidtv_mux *m = kzalloc(sizeof(*m), GFP_KERNEL);

	m->dev = dev;
	m->fe = fe;
	m->timing.pcr_period_usecs = args.pcr_period_usecs;
	m->timing.si_period_usecs  = args.si_period_usecs;

	m->mux_rate_kbytes_sec = args.mux_rate_kbytes_sec;

	m->on_new_packets_available_cb = args.on_new_packets_available_cb;

	m->mux_buf = vzalloc(args.mux_buf_sz);
	m->mux_buf_sz = args.mux_buf_sz;

	m->pcr_pid = args.pcr_pid;
	m->transport_stream_id = args.transport_stream_id;
	m->priv = args.priv;

	if (args.channels)
		m->channels = args.channels;
	else
		vidtv_channels_init(m);

	/* will alloc data for pmt_sections after initializing pat */
	vidtv_channel_si_init(m);

	INIT_WORK(&m->mpeg_thread, vidtv_mux_tick);

	vidtv_mux_pid_ctx_init(m);

	return m;
}

void vidtv_mux_destroy(struct vidtv_mux *m)
{
	vidtv_mux_stop_thread(m);
	vidtv_mux_pid_ctx_destroy(m);
	vidtv_channel_si_destroy(m);
	vidtv_channels_destroy(m);
	vfree(m->mux_buf);
	kfree(m);
}
