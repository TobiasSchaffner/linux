/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/bitmap.h>
#include <evl/net/skb.h>
#include <evl/net/input.h>
#include <evl/net/packet.h>
#include <evl/net/ipv4.h>
#include <evl/net/timestamping.h>

static DECLARE_BITMAP(vlan_map, VLAN_N_VID);

static struct evl_net_handler evl_net_ether;

static bool pop_vlan_header(struct sk_buff *skb)
{
	int ret;

	/*
	 * Make sure the issuing netdev called eth_type_trans() on the
	 * incoming packet, otherwise something is definitely wrong
	 * there and we should leave inband deal with the mess.
	 */
	if (unlikely(!skb_mac_header_was_set(skb)))
		return false;

	/* Drivers should never send us cloned oob skbs. */
	EVL_WARN_ON_ONCE(NET, skb_cloned(skb));
	skb_push_rcsum(skb, ETH_HLEN);
	ret = skb_vlan_pop(skb);
	skb_pull_rcsum(skb, ETH_HLEN);
	if (EVL_WARN_ON_ONCE(NET, ret))
		return false;

	return true;
}

/**
 * evl_net_ether_accept - Unconditionally accept an ethernet packet
 * for the out-of-band stack, stripping out the VLAN information if
 * present.
 *
 * @skb the packet to deliver. May be linked to some upstream queue.
 */
bool evl_net_ether_accept(struct sk_buff *skb)
{
	/*
	 * If VLAN (un)tagging is not hw-accelerated, pop the VLAN
	 * header manually.
	 */
	if (!skb_vlan_tag_present(skb) && !pop_vlan_header(skb))
		return false;

	evl_net_receive(skb, &evl_net_ether);

	return true;
}

/*
 * Check whether we should consider the packet for VLAN-based
 * selection, fetching the TCI we are interested in if so. We handle
 * IPv4 with 802.1Q or 802.1ad (QinQ) encapsulation. In the latter
 * case, the encapsulated protocol and TCI data we look for are
 * carried by the inner 802.1Q header. We do not alter the input
 * packet.
 */
static bool accept_vlan_encap(struct sk_buff *skb, u16 *vlan_tci)
{
	struct vlan_ethhdr *ehdr = (struct vlan_ethhdr *)skb_mac_header(skb);
	struct vlan_hdr *inner;

	if (!eth_type_vlan(skb->protocol))
		return false;

	if (skb->len < VLAN_ETH_HLEN)
		return false;	/* Uhh?? */

	switch (ehdr->h_vlan_encapsulated_proto) {
	case htons(ETH_P_IP):	/* simple 802.1Q encapsulation. */
		*vlan_tci = ntohs(ehdr->h_vlan_TCI);
		return true;
	case htons(ETH_P_8021Q): /* nested QinQ encapsulation. */
		if (skb->len < VLAN_ETH_HLEN + VLAN_HLEN)
			return false;
		inner = (struct vlan_hdr *)(ehdr + 1);
		*vlan_tci = ntohs(inner->h_vlan_TCI);
		return inner->h_vlan_encapsulated_proto == htons(ETH_P_IP);
	}

	return false;
}

/**
 * evl_net_ether_accept_vlan - Accept an IPv4 packet if it flows
 * through an out-of-band VLAN channel.
 *
 * Decide whether an incoming packet should be handled by the
 * out-of-band networking stack instead of the in-band one. This
 * routine checks whether some VLAN information stored into the packet
 * matches one of the VIDs reserved for out-of-band traffic.
 *
 * This routine accepts VLAN packets (802.1Q and 802.1ad)
 * encapsulating IPv4 packets only, so that other payload types we
 * don't deal with always flow through the inband stack
 * (e.g. ETH_P_ARP).
 *
 * @skb the packet to deliver. May be linked to some upstream queue.
 *
 * Returns true if the packet was queued for the out-of-band stack to
 * handle it.
 */
bool evl_net_ether_accept_vlan(struct sk_buff *skb)
{
	u16 vlan_tci;

	/* Try the accelerated way first. */
	if (likely(!__vlan_hwaccel_get_tag(skb, &vlan_tci))) {
		if (skb->protocol != htons(ETH_P_IP))
			return false;

		if (!test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
			return false; /* Not an out-of-band channel. */
	} else {
		/*
		 * Deal manually with input from adapters without hw
		 * accelerated VLAN processing. We only peek at the
		 * packet to figure out whether it may flow through an
		 * out-of-band VLAN channel, in which case we pop the
		 * VLAN header(s) before queuing it for processing.
		 */
		if (!accept_vlan_encap(skb, &vlan_tci))
			return false;

		/* Check the VLAN channel proper. */
		if (!test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
			return false;

		/*
		 * We are going to accept this packet for out-of-band
		 * handling, pop the VLAN header(s) before queuing it
		 * for RX.
		 */
		if (!pop_vlan_header(skb))
			return false;
	}

	evl_net_receive(skb, &evl_net_ether);

	return true;
}

/**
 *	net_ether_ingress - pass an ethernet packet upward to the
 *	stack.
 *
 *	We are called from the RX kthread from oob context, hard irqs
 *	on.  skb is not linked to any queue.
 */
static void net_ether_ingress(struct sk_buff *skb) /* oob */
{
	/* Try to deliver to a raw packet socket first. */
	if (evl_net_packet_deliver(skb))
		return;

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		if (likely(!evl_net_ipv4_deliver(skb)))
			return;
		/* Something went wrong at delivery, drop it. */
		fallthrough;
	default:
		/* Drop any packet from protocols we don't support. */
	}

	evl_net_free_skb(skb);
}

static struct evl_net_handler evl_net_ether = {
	.ingress = net_ether_ingress,
};

static inline bool contains_reserved_vid(unsigned long *map)
{
	/* VID 0, 1 and 4095 are reserved. */
	return test_bit(0, map) || test_bit(1, map) || test_bit(VLAN_VID_MASK, map);
}

ssize_t evl_net_store_vlans(const char *buf, size_t len)
{
	unsigned long *new_map;
	ssize_t ret;

	new_map = bitmap_zalloc(VLAN_N_VID, GFP_KERNEL);
	if (new_map == NULL)
		return -ENOMEM;

	ret = bitmap_parselist(buf, new_map, VLAN_N_VID);
	if (!ret && contains_reserved_vid(new_map))
		ret = -EINVAL;
	if (ret) {
		bitmap_free(new_map);
		return ret;
	}

	/*
	 * We don't have to provide for atomic update wrt our net
	 * stack when updating the vlan map. We use the VID as a
	 * shortlived information early for filtering
	 * input. Serializing writes/stores which the vfs does for us
	 * is enough.
	 */
	bitmap_copy(vlan_map, new_map, VLAN_N_VID);
	bitmap_free(new_map);

	return len;
}

ssize_t evl_net_show_vlans(char *buf, size_t len)
{
	return scnprintf(buf, len, "%*pbl\n", VLAN_N_VID, vlan_map);
}
