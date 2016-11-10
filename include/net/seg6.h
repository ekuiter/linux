/*
 *  SR-IPv6 implementation
 *
 *  Author:
 *  David Lebrun <david.lebrun@uclouvain.be>
 *
 *
 *  This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _NET_SEG6_H
#define _NET_SEG6_H

static inline void update_csum_diff4(struct sk_buff *skb, __be32 from,
				     __be32 to)
{
	__be32 diff[] = { ~from, to };

	skb->csum = ~csum_partial((char *)diff, sizeof(diff), ~skb->csum);
}

static inline void update_csum_diff16(struct sk_buff *skb, __be32 *from,
				      __be32 *to)
{
	__be32 diff[] = {
		~from[0], ~from[1], ~from[2], ~from[3],
		to[0], to[1], to[2], to[3],
	};

	skb->csum = ~csum_partial((char *)diff, sizeof(diff), ~skb->csum);
}

#endif
