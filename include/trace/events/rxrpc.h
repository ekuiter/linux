/* AF_RXRPC tracepoints
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rxrpc

#if !defined(_TRACE_RXRPC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RXRPC_H

#include <linux/tracepoint.h>

TRACE_EVENT(rxrpc_conn,
	    TP_PROTO(struct rxrpc_connection *conn, enum rxrpc_conn_trace op,
		     int usage, const void *where),

	    TP_ARGS(conn, op, usage, where),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_connection *,	conn		)
		    __field(int,			op		)
		    __field(int,			usage		)
		    __field(const void *,		where		)
			     ),

	    TP_fast_assign(
		    __entry->conn = conn;
		    __entry->op = op;
		    __entry->usage = usage;
		    __entry->where = where;
			   ),

	    TP_printk("C=%p %s u=%d sp=%pSR",
		      __entry->conn,
		      rxrpc_conn_traces[__entry->op],
		      __entry->usage,
		      __entry->where)
	    );

TRACE_EVENT(rxrpc_client,
	    TP_PROTO(struct rxrpc_connection *conn, int channel,
		     enum rxrpc_client_trace op),

	    TP_ARGS(conn, channel, op),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_connection *,	conn		)
		    __field(u32,			cid		)
		    __field(int,			channel		)
		    __field(int,			usage		)
		    __field(enum rxrpc_client_trace,	op		)
		    __field(enum rxrpc_conn_cache_state, cs		)
			     ),

	    TP_fast_assign(
		    __entry->conn = conn;
		    __entry->channel = channel;
		    __entry->usage = atomic_read(&conn->usage);
		    __entry->op = op;
		    __entry->cid = conn->proto.cid;
		    __entry->cs = conn->cache_state;
			   ),

	    TP_printk("C=%p h=%2d %s %s i=%08x u=%d",
		      __entry->conn,
		      __entry->channel,
		      rxrpc_client_traces[__entry->op],
		      rxrpc_conn_cache_states[__entry->cs],
		      __entry->cid,
		      __entry->usage)
	    );

TRACE_EVENT(rxrpc_call,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_call_trace op,
		     int usage, const void *where, const void *aux),

	    TP_ARGS(call, op, usage, where, aux),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(int,			op		)
		    __field(int,			usage		)
		    __field(const void *,		where		)
		    __field(const void *,		aux		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->op = op;
		    __entry->usage = usage;
		    __entry->where = where;
		    __entry->aux = aux;
			   ),

	    TP_printk("c=%p %s u=%d sp=%pSR a=%p",
		      __entry->call,
		      rxrpc_call_traces[__entry->op],
		      __entry->usage,
		      __entry->where,
		      __entry->aux)
	    );

TRACE_EVENT(rxrpc_skb,
	    TP_PROTO(struct sk_buff *skb, enum rxrpc_skb_trace op,
		     int usage, int mod_count, const void *where),

	    TP_ARGS(skb, op, usage, mod_count, where),

	    TP_STRUCT__entry(
		    __field(struct sk_buff *,		skb		)
		    __field(enum rxrpc_skb_trace,	op		)
		    __field(int,			usage		)
		    __field(int,			mod_count	)
		    __field(const void *,		where		)
			     ),

	    TP_fast_assign(
		    __entry->skb = skb;
		    __entry->op = op;
		    __entry->usage = usage;
		    __entry->mod_count = mod_count;
		    __entry->where = where;
			   ),

	    TP_printk("s=%p %s u=%d m=%d p=%pSR",
		      __entry->skb,
		      rxrpc_skb_traces[__entry->op],
		      __entry->usage,
		      __entry->mod_count,
		      __entry->where)
	    );

TRACE_EVENT(rxrpc_rx_packet,
	    TP_PROTO(struct rxrpc_skb_priv *sp),

	    TP_ARGS(sp),

	    TP_STRUCT__entry(
		    __field_struct(struct rxrpc_host_header,	hdr		)
			     ),

	    TP_fast_assign(
		    memcpy(&__entry->hdr, &sp->hdr, sizeof(__entry->hdr));
			   ),

	    TP_printk("%08x:%08x:%08x:%04x %08x %08x %02x %02x %s",
		      __entry->hdr.epoch, __entry->hdr.cid,
		      __entry->hdr.callNumber, __entry->hdr.serviceId,
		      __entry->hdr.serial, __entry->hdr.seq,
		      __entry->hdr.type, __entry->hdr.flags,
		      __entry->hdr.type <= 15 ? rxrpc_pkts[__entry->hdr.type] : "?UNK")
	    );

TRACE_EVENT(rxrpc_rx_done,
	    TP_PROTO(int result, int abort_code),

	    TP_ARGS(result, abort_code),

	    TP_STRUCT__entry(
		    __field(int,			result		)
		    __field(int,			abort_code	)
			     ),

	    TP_fast_assign(
		    __entry->result = result;
		    __entry->abort_code = abort_code;
			   ),

	    TP_printk("r=%d a=%d", __entry->result, __entry->abort_code)
	    );

TRACE_EVENT(rxrpc_abort,
	    TP_PROTO(const char *why, u32 cid, u32 call_id, rxrpc_seq_t seq,
		     int abort_code, int error),

	    TP_ARGS(why, cid, call_id, seq, abort_code, error),

	    TP_STRUCT__entry(
		    __array(char,			why, 4		)
		    __field(u32,			cid		)
		    __field(u32,			call_id		)
		    __field(rxrpc_seq_t,		seq		)
		    __field(int,			abort_code	)
		    __field(int,			error		)
			     ),

	    TP_fast_assign(
		    memcpy(__entry->why, why, 4);
		    __entry->cid = cid;
		    __entry->call_id = call_id;
		    __entry->abort_code = abort_code;
		    __entry->error = error;
		    __entry->seq = seq;
			   ),

	    TP_printk("%08x:%08x s=%u a=%d e=%d %s",
		      __entry->cid, __entry->call_id, __entry->seq,
		      __entry->abort_code, __entry->error, __entry->why)
	    );

TRACE_EVENT(rxrpc_transmit,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_transmit_trace why),

	    TP_ARGS(call, why),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(enum rxrpc_transmit_trace,	why		)
		    __field(rxrpc_seq_t,		tx_hard_ack	)
		    __field(rxrpc_seq_t,		tx_top		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->why = why;
		    __entry->tx_hard_ack = call->tx_hard_ack;
		    __entry->tx_top = call->tx_top;
			   ),

	    TP_printk("c=%p %s f=%08x n=%u",
		      __entry->call,
		      rxrpc_transmit_traces[__entry->why],
		      __entry->tx_hard_ack + 1,
		      __entry->tx_top - __entry->tx_hard_ack)
	    );

TRACE_EVENT(rxrpc_rx_ack,
	    TP_PROTO(struct rxrpc_call *call, rxrpc_seq_t first, u8 reason, u8 n_acks),

	    TP_ARGS(call, first, reason, n_acks),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(rxrpc_seq_t,		first		)
		    __field(u8,				reason		)
		    __field(u8,				n_acks		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->first = first;
		    __entry->reason = reason;
		    __entry->n_acks = n_acks;
			   ),

	    TP_printk("c=%p %s f=%08x n=%u",
		      __entry->call,
		      rxrpc_ack_names[__entry->reason],
		      __entry->first,
		      __entry->n_acks)
	    );

TRACE_EVENT(rxrpc_tx_data,
	    TP_PROTO(struct rxrpc_call *call, rxrpc_seq_t seq,
		     rxrpc_serial_t serial, u8 flags, bool lose),

	    TP_ARGS(call, seq, serial, flags, lose),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(rxrpc_seq_t,		seq		)
		    __field(rxrpc_serial_t,		serial		)
		    __field(u8,				flags		)
		    __field(bool,			lose		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->seq = seq;
		    __entry->serial = serial;
		    __entry->flags = flags;
		    __entry->lose = lose;
			   ),

	    TP_printk("c=%p DATA %08x q=%08x fl=%02x%s",
		      __entry->call,
		      __entry->serial,
		      __entry->seq,
		      __entry->flags,
		      __entry->lose ? " *LOSE*" : "")
	    );

TRACE_EVENT(rxrpc_tx_ack,
	    TP_PROTO(struct rxrpc_call *call, rxrpc_serial_t serial,
		     rxrpc_seq_t ack_first, rxrpc_serial_t ack_serial,
		     u8 reason, u8 n_acks),

	    TP_ARGS(call, serial, ack_first, ack_serial, reason, n_acks),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(rxrpc_serial_t,		serial		)
		    __field(rxrpc_seq_t,		ack_first	)
		    __field(rxrpc_serial_t,		ack_serial	)
		    __field(u8,				reason		)
		    __field(u8,				n_acks		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->serial = serial;
		    __entry->ack_first = ack_first;
		    __entry->ack_serial = ack_serial;
		    __entry->reason = reason;
		    __entry->n_acks = n_acks;
			   ),

	    TP_printk(" c=%p ACK  %08x %s f=%08x r=%08x n=%u",
		      __entry->call,
		      __entry->serial,
		      rxrpc_ack_names[__entry->reason],
		      __entry->ack_first,
		      __entry->ack_serial,
		      __entry->n_acks)
	    );

TRACE_EVENT(rxrpc_receive,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_receive_trace why,
		     rxrpc_serial_t serial, rxrpc_seq_t seq),

	    TP_ARGS(call, why, serial, seq),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(enum rxrpc_receive_trace,	why		)
		    __field(rxrpc_serial_t,		serial		)
		    __field(rxrpc_seq_t,		seq		)
		    __field(rxrpc_seq_t,		hard_ack	)
		    __field(rxrpc_seq_t,		top		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->why = why;
		    __entry->serial = serial;
		    __entry->seq = seq;
		    __entry->hard_ack = call->rx_hard_ack;
		    __entry->top = call->rx_top;
			   ),

	    TP_printk("c=%p %s r=%08x q=%08x w=%08x-%08x",
		      __entry->call,
		      rxrpc_receive_traces[__entry->why],
		      __entry->serial,
		      __entry->seq,
		      __entry->hard_ack,
		      __entry->top)
	    );

TRACE_EVENT(rxrpc_recvmsg,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_recvmsg_trace why,
		     rxrpc_seq_t seq, unsigned int offset, unsigned int len,
		     int ret),

	    TP_ARGS(call, why, seq, offset, len, ret),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(enum rxrpc_recvmsg_trace,	why		)
		    __field(rxrpc_seq_t,		seq		)
		    __field(unsigned int,		offset		)
		    __field(unsigned int,		len		)
		    __field(int,			ret		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->why = why;
		    __entry->seq = seq;
		    __entry->offset = offset;
		    __entry->len = len;
		    __entry->ret = ret;
			   ),

	    TP_printk("c=%p %s q=%08x o=%u l=%u ret=%d",
		      __entry->call,
		      rxrpc_recvmsg_traces[__entry->why],
		      __entry->seq,
		      __entry->offset,
		      __entry->len,
		      __entry->ret)
	    );

TRACE_EVENT(rxrpc_rtt_tx,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_rtt_tx_trace why,
		     rxrpc_serial_t send_serial),

	    TP_ARGS(call, why, send_serial),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(enum rxrpc_rtt_tx_trace,	why		)
		    __field(rxrpc_serial_t,		send_serial	)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->why = why;
		    __entry->send_serial = send_serial;
			   ),

	    TP_printk("c=%p %s sr=%08x",
		      __entry->call,
		      rxrpc_rtt_tx_traces[__entry->why],
		      __entry->send_serial)
	    );

TRACE_EVENT(rxrpc_rtt_rx,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_rtt_rx_trace why,
		     rxrpc_serial_t send_serial, rxrpc_serial_t resp_serial,
		     s64 rtt, u8 nr, s64 avg),

	    TP_ARGS(call, why, send_serial, resp_serial, rtt, nr, avg),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,	call		)
		    __field(enum rxrpc_rtt_rx_trace,	why		)
		    __field(u8,				nr		)
		    __field(rxrpc_serial_t,		send_serial	)
		    __field(rxrpc_serial_t,		resp_serial	)
		    __field(s64,			rtt		)
		    __field(u64,			avg		)
			     ),

	    TP_fast_assign(
		    __entry->call = call;
		    __entry->why = why;
		    __entry->send_serial = send_serial;
		    __entry->resp_serial = resp_serial;
		    __entry->rtt = rtt;
		    __entry->nr = nr;
		    __entry->avg = avg;
			   ),

	    TP_printk("c=%p %s sr=%08x rr=%08x rtt=%lld nr=%u avg=%lld",
		      __entry->call,
		      rxrpc_rtt_rx_traces[__entry->why],
		      __entry->send_serial,
		      __entry->resp_serial,
		      __entry->rtt,
		      __entry->nr,
		      __entry->avg)
	    );

TRACE_EVENT(rxrpc_timer,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_timer_trace why,
		     unsigned long now),

	    TP_ARGS(call, why, now),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,		call		)
		    __field(enum rxrpc_timer_trace,		why		)
		    __field(unsigned long,			now		)
		    __field(unsigned long,			expire_at	)
		    __field(unsigned long,			ack_at		)
		    __field(unsigned long,			resend_at	)
		    __field(unsigned long,			timer		)
			     ),

	    TP_fast_assign(
		    __entry->call	= call;
		    __entry->why	= why;
		    __entry->now	= now;
		    __entry->expire_at	= call->expire_at;
		    __entry->ack_at	= call->ack_at;
		    __entry->resend_at	= call->resend_at;
		    __entry->timer	= call->timer.expires;
			   ),

	    TP_printk("c=%p %s now=%lx x=%ld a=%ld r=%ld t=%ld",
		      __entry->call,
		      rxrpc_timer_traces[__entry->why],
		      __entry->now,
		      __entry->expire_at - __entry->now,
		      __entry->ack_at - __entry->now,
		      __entry->resend_at - __entry->now,
		      __entry->timer - __entry->now)
	    );

TRACE_EVENT(rxrpc_rx_lose,
	    TP_PROTO(struct rxrpc_skb_priv *sp),

	    TP_ARGS(sp),

	    TP_STRUCT__entry(
		    __field_struct(struct rxrpc_host_header,	hdr		)
			     ),

	    TP_fast_assign(
		    memcpy(&__entry->hdr, &sp->hdr, sizeof(__entry->hdr));
			   ),

	    TP_printk("%08x:%08x:%08x:%04x %08x %08x %02x %02x %s *LOSE*",
		      __entry->hdr.epoch, __entry->hdr.cid,
		      __entry->hdr.callNumber, __entry->hdr.serviceId,
		      __entry->hdr.serial, __entry->hdr.seq,
		      __entry->hdr.type, __entry->hdr.flags,
		      __entry->hdr.type <= 15 ? rxrpc_pkts[__entry->hdr.type] : "?UNK")
	    );

TRACE_EVENT(rxrpc_propose_ack,
	    TP_PROTO(struct rxrpc_call *call, enum rxrpc_propose_ack_trace why,
		     u8 ack_reason, rxrpc_serial_t serial, bool immediate,
		     bool background, enum rxrpc_propose_ack_outcome outcome),

	    TP_ARGS(call, why, ack_reason, serial, immediate, background,
		    outcome),

	    TP_STRUCT__entry(
		    __field(struct rxrpc_call *,		call		)
		    __field(enum rxrpc_propose_ack_trace,	why		)
		    __field(rxrpc_serial_t,			serial		)
		    __field(u8,					ack_reason	)
		    __field(bool,				immediate	)
		    __field(bool,				background	)
		    __field(enum rxrpc_propose_ack_outcome,	outcome		)
			     ),

	    TP_fast_assign(
		    __entry->call	= call;
		    __entry->why	= why;
		    __entry->serial	= serial;
		    __entry->ack_reason	= ack_reason;
		    __entry->immediate	= immediate;
		    __entry->background	= background;
		    __entry->outcome	= outcome;
			   ),

	    TP_printk("c=%p %s %s r=%08x i=%u b=%u%s",
		      __entry->call,
		      rxrpc_propose_ack_traces[__entry->why],
		      rxrpc_ack_names[__entry->ack_reason],
		      __entry->serial,
		      __entry->immediate,
		      __entry->background,
		      rxrpc_propose_ack_outcomes[__entry->outcome])
	    );

#endif /* _TRACE_RXRPC_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
