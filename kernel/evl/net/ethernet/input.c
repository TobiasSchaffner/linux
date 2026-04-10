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
#include <evl/net/device.h>
#include <evl/net/ipv4.h>
#include <evl/net/timestamping.h>

static DECLARE_BITMAP(vlan_map, VLAN_N_VID);

static struct evl_net_handler evl_net_ether;

static void ether_receive(struct sk_buff *skb)
{
	struct evl_netdev_stats *stats;
	struct net_device *vlan_dev;

	if (skb_vlan_tag_present(skb)) {
		vlan_dev = evl_net_find_vlan_dev(
			skb->dev,
			skb->vlan_proto, skb_vlan_tag_get_id(skb));
		if (likely(vlan_dev)) {
			skb->dev = vlan_dev;
			stats = evl_net_get_stats(vlan_dev);
			evl_counter_inc_careful(&stats->rx_packets);
			evl_counter_add_careful(&stats->rx_bytes, skb->len);
		}
	}

	evl_net_receive(skb, &evl_net_ether);
}

/*
 * Pop any VLAN header from the packet, checking for an IPv4 payload
 * flowing through an out-of-band VLAN.  We handle 802.1Q and 802.1ad
 * (QinQ) encapsulation.
 *
 * This routine expects the MAC header to be set in @skb.
 *
 * Returns an action code to the caller:
 *
 * - RX_ACCEPT if the packet may be accepted. The VLAN header is
 *   stripped on return.
 *
 * - RX_SKIP if the packet does not flow on an out-of-band VLAN.
 *
 * - RX_DROP if the packet looks broken and should be dropped
 *   (e.g. bad VLAN header).
 */
static enum evl_net_rx_action pop_vlan_header(struct sk_buff *skb)
{
	struct vlan_ethhdr *ehdr;
	struct vlan_hdr *inner;
	__be16 vlan_proto;
	u16 vlan_tci;
	int ret;

	/* Try the accelerated way first. */
	if (likely(!__vlan_hwaccel_get_tag(skb, &vlan_tci))) {
		if (skb->protocol != htons(ETH_P_IP))
			return EVL_RX_SKIP;

		if (!test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
			return EVL_RX_SKIP;

		return EVL_RX_ACCEPT;
	}

	/*
	 * Deal manually with input from adapters without hw
	 * accelerated VLAN processing.
	 */

	if (EVL_WARN_ON_ONCE(NET, !skb_mac_header_was_set(skb)))
		return EVL_RX_DROP; /* Broken driver. */

	if (!eth_type_vlan(skb->protocol))
		return EVL_RX_SKIP;

	if (skb->len < VLAN_ETH_HLEN)
		return EVL_RX_DROP;

	vlan_proto = skb->protocol;
	ehdr = (struct vlan_ethhdr *)skb_mac_header(skb);

	switch (ehdr->h_vlan_encapsulated_proto) {
	case htons(ETH_P_IP):	/* simple 802.1Q encapsulation. */
		vlan_tci = ntohs(ehdr->h_vlan_TCI);
		break;
	case htons(ETH_P_8021Q): /* nested QinQ encapsulation. */
		if (skb->len < VLAN_ETH_HLEN + VLAN_HLEN)
			return EVL_RX_DROP;
		inner = (struct vlan_hdr *)(ehdr + 1);
		if (inner->h_vlan_encapsulated_proto != htons(ETH_P_IP))
			return EVL_RX_SKIP;
		vlan_tci = ntohs(inner->h_vlan_TCI);
		break;
	}

	if (!test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
		return EVL_RX_SKIP;

	/* Drivers should never send us cloned oob skbs. */
	EVL_WARN_ON_ONCE(NET, skb_cloned(skb));
	skb_push_rcsum(skb, ETH_HLEN);
	ret = skb_vlan_pop(skb);
	skb_pull_rcsum(skb, ETH_HLEN);
	if (EVL_WARN_ON_ONCE(NET, ret))
		return EVL_RX_DROP;

	/* For ether_receive() to set skb->dev appropriately. */
	__vlan_hwaccel_put_tag(skb, vlan_proto, vlan_tci);

	return EVL_RX_ACCEPT;
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
 * Returns an action code to the caller:
 *
 * - RX_ACCEPT if the packet was indeed queued for the out-of-band
 *   stack to handle it.
 *
 * - RX_SKIP if the packet should be handed over to the in-band stack
 *   eventually.
 *
 * - RX_DROP if the packet looks broken and should be dropped.
 */
enum evl_net_rx_action evl_net_ether_accept_vlan(struct sk_buff *skb)
{
	enum evl_net_rx_action ret = pop_vlan_header(skb);

	if (likely(ret == EVL_RX_ACCEPT))
		ether_receive(skb);

	return ret;
}

/**
 * evl_net_ether_accept - Accept an ethernet packet.
 *
 * A variant of evl_net_ether_accept_vlan() which conditionally
 * filters the input on the VLAN tag only when the latter is present,
 * in which case an out-of-band VLAN must be found.
 *
 * Non-IPv4 packets are never passed to the oob stack, so that other
 * payload types we don't deal with always flow through the inband
 * stack (e.g. ETH_P_ARP).
 *
 * @skb the packet to deliver. May be linked to some upstream queue.
 */
enum evl_net_rx_action evl_net_ether_accept(struct sk_buff *skb)
{
	enum evl_net_rx_action ret;

	if (eth_type_vlan(skb->protocol)) {
		ret = pop_vlan_header(skb);
		if (ret != EVL_RX_ACCEPT)
			return ret;
	} else {
		if (skb->protocol != htons(ETH_P_IP))
			return EVL_RX_SKIP;
	}

	ether_receive(skb);

	return EVL_RX_ACCEPT;
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
