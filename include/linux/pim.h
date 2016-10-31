#ifndef __LINUX_PIM_H
#define __LINUX_PIM_H

#include <linux/skbuff.h>
#include <asm/byteorder.h>

/* Message types - V1 */
#define PIM_V1_VERSION		cpu_to_be32(0x10000000)
#define PIM_V1_REGISTER		1

/* Message types - V2 */
#define PIM_VERSION		2
#define PIM_REGISTER		1

#define PIM_NULL_REGISTER	cpu_to_be32(0x40000000)

/* RFC7761, sec 4.9:
 * The PIM header common to all PIM messages is:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |PIM Ver| Type  |   Reserved    |           Checksum            |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct pimhdr {
	__u8	type;
	__u8	reserved;
	__be16	csum;
};

/* PIMv2 register message header layout (ietf-draft-idmr-pimvsm-v2-00.ps */
struct pimreghdr {
	__u8	type;
	__u8	reserved;
	__be16	csum;
	__be32	flags;
};

int pim_rcv_v1(struct sk_buff *skb);

static inline bool ipmr_pimsm_enabled(void)
{
	return IS_BUILTIN(CONFIG_IP_PIMSM_V1) || IS_BUILTIN(CONFIG_IP_PIMSM_V2);
}

static inline struct pimhdr *pim_hdr(const struct sk_buff *skb)
{
	return (struct pimhdr *)skb_transport_header(skb);
}

static inline u8 pim_hdr_version(const struct pimhdr *pimhdr)
{
	return pimhdr->type >> 4;
}

static inline u8 pim_hdr_type(const struct pimhdr *pimhdr)
{
	return pimhdr->type & 0xf;
}
#endif
