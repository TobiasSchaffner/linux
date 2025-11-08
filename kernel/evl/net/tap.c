/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/inband_work.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <evl/net/skb.h>
#include <evl/net/tap.h>

#define EVL_TAP_RING_SIZE  2048

struct evl_net_tap_data {
	struct net_device *dev;
	struct sk_buff *skb;
};

static void feed_tap_in(struct net_device *dev, struct sk_buff *skb)
{
	/*
	 * The buffer headers are reset and untagged from VLAN bits if
	 * any as well (see netif_deliver_oob()).
	 */
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
	dev_queue_xmit_nit(skb, dev);
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
