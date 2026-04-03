/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/inband_work.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <evl/net/skb.h>
#include <evl/net/tap.h>

#define EVL_TAP_RING_SIZE  4096

struct evl_net_tap_data {
	struct net_device *dev;
	struct sk_buff *skb;
};

static void feed_tap_in(struct net_device *dev, struct sk_buff *skb)
{
	struct sk_buff *bskb;

	if (EVL_WARN_ON(NET, !skb_mac_header_was_set(skb)))
		return;

	lockdep_assert_in_softirq();

	/*
	 * If receiving from a VLAN device, attempt to feed the tap of
	 * its base device as well. Allocation failure is not deemed
	 * critical (although most likely to go worse soon enough),
	 * just skip this part.
	 */
	if (is_vlan_dev(dev)) {
		skb_push(skb, skb->mac_len); /* Restore the MAC header. */

		/* Add back a VLAN header to a copy of skb. */
		bskb = pskb_copy(skb, GFP_ATOMIC);
		if (unlikely(!bskb))
			goto unlucky;

		bskb->dev = vlan_dev_real_dev(dev);
		bskb = __vlan_hwaccel_push_inside(bskb);
		if (likely(bskb)) {
			skb_reset_mac_len(bskb);
			dev_queue_recv_nit(bskb, bskb->dev);
		}

		kfree_skb(bskb);
	unlucky:
		__skb_pull(skb, skb->mac_len);

		/*
		 * Make sure that a packet sniffer reading the input
		 * tap of the VLAN device won't display the VLAN
		 * encapsulation.
		 */
		__vlan_hwaccel_clear_tag(skb);
	}

	dev_queue_recv_nit(skb, dev);
}

static void deliver_input_nit(struct evl_net_tap_data *data)
{
	local_bh_disable();
	feed_tap_in(data->dev, data->skb);
	local_bh_enable();
	evl_net_free_skb(data->skb);
}

INBAND_BATCH_WORK(
	evl_net_tap_in, deliver_input_nit,
	struct evl_net_tap_data, EVL_TAP_RING_SIZE
);

static DECLARE_INBAND_BATCH_WORK(evl_net_tap_in, tap_in);

/*
 * Feed the input stream of a tap. BH must be held.
 */
void evl_net_tap_in(struct net_device *dev, struct sk_buff *skb)
{
	struct evl_net_tap_data in;
	struct sk_buff *qskb;

	/*
	 * If @dev is not oob-capable, we are already running in-band,
	 * feed the nits via the in-band call immediately.
	 */
	if (running_inband()) {
		feed_tap_in(dev, skb);
		return;
	}

	/*
	 * The guarantee is that we feed taps only with skbs we did
	 * accept for oob handling (see netif_deliver_oob()),
	 * therefore skb_oob_clone() is by design able to deal with
	 * any type of buffers the oob stack accepts.
	 */
	qskb = skb_oob_clone(skb);
	/* Failing to feed taps not considered harmful. */
	if (unlikely(!qskb))
		return;

	/*
	 * Since we cloned the skb using a buffer from the device
	 * pool, that device won't vanish until the buffer is released
	 * (see wmem crossing).
	 */
	in.dev = dev;
	in.skb = qskb;
	queue_evl_net_tap_in(&tap_in, &in);
}

static void feed_tap_out(struct net_device *dev, struct sk_buff *skb)
{
	struct sk_buff *bskb;

	if (!is_vlan_dev(dev))
		goto do_basedev;

	if (EVL_WARN_ON(NET, !skb_mac_header_was_set(skb)))
		goto no_vlan;

	/* Strip the VLAN header before sending to the VLAN tap. */
	bskb = pskb_copy(skb, GFP_ATOMIC);
	if (unlikely(!bskb))
		goto no_vlan;

	if (!skb_vlan_pop(bskb)) {
		bskb->dev = dev;
		local_bh_disable();
		dev_queue_xmit_nit(bskb, dev);
		local_bh_enable();
	}

	kfree_skb(bskb);
no_vlan:
	dev = vlan_dev_real_dev(dev);
do_basedev:
	local_bh_disable();
	dev_queue_xmit_nit(skb, dev);
	local_bh_enable();
}

static void deliver_output_nit(struct evl_net_tap_data *data)
{
	feed_tap_out(data->dev, data->skb);
	evl_net_free_skb(data->skb);
}

INBAND_BATCH_WORK(
	evl_net_tap_out, deliver_output_nit,
	struct evl_net_tap_data, EVL_TAP_RING_SIZE
);

static DECLARE_INBAND_BATCH_WORK(evl_net_tap_out, tap_out);

void evl_net_tap_out(struct net_device *dev, struct sk_buff *skb)
{
	struct evl_net_tap_data out;
	struct sk_buff *qskb;

	qskb = skb_oob_clone(skb);
	/* Failing to feed taps not considered harmful. */
	if (unlikely(!qskb))
		return;

	out.dev = dev;
	out.skb = qskb;
	queue_evl_net_tap_out(&tap_out, &out);
}

void evl_net_init_taps(void)
{
	init_evl_net_tap_in(&tap_in);
	init_evl_net_tap_out(&tap_out);
}
