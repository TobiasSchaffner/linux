/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/inetdevice.h>
#include <net/inet_sock.h>
#include <net/inet_common.h>
#include <net/icmp.h>
#include <evl/memory.h>
#include <evl/net/skb.h>
#include <evl/net/device.h>
#include <evl/net/timestamping.h>
#include <evl/net/socket.h>
#include <evl/net/ip.h>
#include <evl/net/ipv4.h>
#include <evl/net/ipv4/icmp.h>

static inline __wsum icmp_checksum(struct sk_buff *skb, int offset)
{
	__wsum csum = csum_partial(skb->data + offset, skb->len - offset, 0);
	struct sk_buff *fskb;

	skb_walk_frags(skb, fskb) {
		csum = csum_partial(fskb->data, fskb->len, csum);
	}

	return csum;
}

/*
 * We don't do any output routing when replying to ECHO requests, we
 * bluntly reply to the source IP via the source device.
 */
static int do_echoreply(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb), *riph;
	struct net_device *dev = skb->dev;
	struct net *net = dev_net_rcu(dev);
	char src_hwaddr[MAX_ADDR_LEN];
	struct icmphdr *ricmph;
	struct sk_buff *rskb;
	__be32 raddr;
	int headroom;
	__wsum csum;
	int ret;

	if (READ_ONCE(net->ipv4.sysctl_icmp_echo_ignore_all))
		goto out;

	raddr = iph->daddr;
	if (ipv4_is_lbcast(raddr) || ipv4_is_multicast(raddr)) {
		if (READ_ONCE(net->ipv4.sysctl_icmp_echo_ignore_broadcasts))
			goto out;
		/* Get the replier source address. */
		raddr = evl_net_ipv4_devaddr(dev);
		if (!raddr) {
			evl_net_free_skb(rskb);
			return -EDESTADDRREQ;
		}
	}

	/* Fetch the source MAC address. */
	ret = evl_net_skb_parse(skb, src_hwaddr);
	if (ret <= 0)
		return ret ?: -ENODEV;

	/*
	 * Unfortunately, we have to deep-copy the source packet
	 * content because we need the outgoing one to be pre-mapped
	 * DMA-wise, which is not guaranteed from the ingress
	 * path. This said, this also guarantees a clean state of the
	 * egress-related meta-data.
	 */
	headroom = evl_net_dev_maclen(dev);
	rskb = evl_net_copy_skb(dev, skb, headroom);
	if (IS_ERR(rskb))
		return PTR_ERR(rskb);

	skb_reset_network_header(rskb);

	riph = (struct iphdr *)skb_network_header(rskb);
	riph->saddr = raddr;
	riph->daddr = iph->saddr;
	ip_send_check(riph);

	ricmph = (struct icmphdr *)(riph + 1);
	ricmph->type = ICMP_ECHOREPLY;
	ricmph->checksum = 0;
	csum = icmp_checksum(rskb, (unsigned char *)ricmph - rskb->data);
	ricmph->checksum = csum_fold(csum);

	rskb->ip_summed = CHECKSUM_NONE;

	/* Untransmitted buffers are already released on error. */
	evl_net_dev_transmit(dev, rskb, src_hwaddr);
out:
	evl_net_free_skb(skb);

	return 0;
}

int evl_net_deliver_icmp(struct sk_buff *skb)
{
	int ret;

	if (skb_checksum_simple_validate(skb))
		return -EINVAL;

	/* We receive complete, fully reassembled packets. */

	switch (icmp_hdr(skb)->type) {
	case ICMP_ECHO:
		ret = do_echoreply(skb);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

int evl_net_init_icmp(struct net *net)
{
	return 0;
}

void evl_net_cleanup_icmp(struct net *net)
{
}

static int attach_icmp_socket(struct evl_socket *esk, /* in-band */
			struct evl_net_proto *proto, int protocol)
{
	/* We don't support user ICMP packet I/O yet. */

	return -ENOTSUPP;
}

struct evl_net_proto evl_net_icmp_proto = {
	.attach		= attach_icmp_socket,
	.bind		= evl_socket_no_bind,
	.connect	= evl_socket_no_connect,
	.shutdown	= evl_socket_no_shutdown,
	.ioctl		= evl_socket_no_ioctl,
	.solicit	= evl_socket_no_solicit,
	.oob_send	= evl_socket_no_send,
	.oob_receive	= evl_socket_no_receive,
	.oob_poll	= evl_socket_no_poll,
	.handle_offload	= evl_socket_no_offload,
};
