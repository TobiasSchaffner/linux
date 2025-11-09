/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/irq_work.h>
#include <linux/if_vlan.h>
#include <linux/skbuff.h>
#include <uapi/linux/if_arp.h>
#include <evl/thread.h>
#include <evl/lock.h>
#include <evl/list.h>
#include <evl/flag.h>
#include <evl/net.h>
#include <evl/net/device.h>
#include <evl/net/ipv4.h>
#include <evl/net/timestamping.h>
#include <evl/net/tap.h>

/*
 * NOTE: This code cannot compete with napi_complete_done()
 * with respect to updating the napi state, since the latter
 * only runs for inband traffic received by non oob-capable
 * drivers, and we are exclusively dealing with oob traffic
 * received by oob-capable drivers in this routine.
 */
static void do_poll(struct evl_netdev_state *est) /* oob */
{
	struct napi_struct *n, *tmp;
	unsigned long flags;
	LIST_HEAD(repoll);
	LIST_HEAD(poll);

	raw_spin_lock_irqsave(&est->napi_lock, flags);
	list_splice_init(&est->napi_poll, &poll);
	/*
	 * We might race with __set_rx_filter(), so use atomic bitops
	 * for raising the RX_SCHED flag.
	 */
	clear_bit(EVL_NETDEV_RX_SCHED_BIT, &est->flags);
	raw_spin_unlock_irqrestore(&est->napi_lock, flags);

	list_for_each_entry_safe(n, tmp, &poll, poll_list)
		/* We don't cap the device budget for oob-driven traffic. */
		napi_poll_oob(n, &repoll);

	if (!list_empty(&repoll)) {
		raw_spin_lock_irqsave(&est->napi_lock, flags);
		list_splice_tail_init(&est->napi_poll, &poll);
		list_splice_tail(&repoll, &poll);
		list_splice(&poll, &est->napi_poll);
		set_bit(EVL_NETDEV_RX_SCHED_BIT, &est->flags);
		raw_spin_unlock_irqrestore(&est->napi_lock, flags);
	}
}

/*
 * RX thread dealing with ingress traffic and garbage collection for
 * stale input fragments. Specifically, this thread handles:
 *
 * - the pending NAPI requests for polling the device for I/O events
 * (napi_schedule_oob())
 *
 * - the polled ingress packets queued by netif_deliver_oob(), passing
 * them over to the proper protocol layer.
 *
 * - the garbage collection to flush the IP fragments which have not
 * been collected in time.
 *
 * Each net device is served by a dedicated RX thread.
 */
void evl_net_do_rx(void *arg)
{
	struct net_device *dev = arg;
	struct evl_netdev_state *est;
	struct sk_buff *skb, *next;
	unsigned int packets_in;
	LIST_HEAD(list);
	u64 bytes_in;
	int ret;

	est = dev->oob_state.estate;

	while (!evl_kthread_should_stop()) {
		while (!test_bit(EVL_NETDEV_RX_SCHED_BIT, &est->flags)) {
			ret = evl_wait_flag(&est->rx_flag);
			if (ret)
				goto flush_out;
		}

		/* Poll oob-capable drivers for feeding rx_packets. */
		do_poll(est);

		/*
		 * Process all queued packets received from ->poll()
		 * handlers via netif_deliver_oob() while running
		 * either from the in-band (RX softirq) or oob stage
		 * (RX thread).
		 */
		if (evl_net_move_skb_queue(&est->rx_packets, &list)) {
			packets_in = 0;
			bytes_in = 0;
			list_for_each_entry_safe(skb, next, &list, list) {
				if (skb_is_oob_timestamped(skb))
					skb_shinfo_oob(skb)->queuing_time = evl_ktime_monotonic();
				list_del(&skb->list);
				packets_in++;
				bytes_in += skb->len;
				EVL_NET_CB(skb)->handler->ingress(skb);
			}
			evl_counter_add_careful(&est->stats.rx_packets, packets_in);
			evl_counter_add_careful(&est->stats.rx_bytes, bytes_in);
		}

		evl_net_ipv4_gc(dev_net(dev));
	}

flush_out:
	/*
	 * Drop unconsumed buffers since the interface goes down,
	 * don't even try to inject them into the stack so that we
	 * won't allow trackers holding references on that device to
	 * be created. Since we've passed the crossing in
	 * disable_oob_port(), this is the only safe thing to do.
	 */
	if (evl_net_move_skb_queue(&est->rx_packets, &list)) {
		list_for_each_entry_safe(skb, next, &list, list) {
			list_del(&skb->list);
			evl_net_free_skb(skb);
		}
	}
}

static inline void wake_rx(struct evl_netdev_state *est)
{
	evl_raise_flag(&est->rx_flag);
}

void evl_net_wake_rx(struct net_device *dev)
{
	struct evl_netdev_state *est = dev->oob_state.estate;

	set_bit(EVL_NETDEV_RX_SCHED_BIT, &est->flags);
	wake_rx(est);
}
EXPORT_SYMBOL_GPL(evl_net_wake_rx);

/**
 * evl_net_receive - schedule an ingress packet for oob handling
 *
 * Schedule an incoming packet for delivery to an oob protocol layer
 * via the RX thread. This is called from an out-of-band packet filter
 * (e.g. evl_net_ether_accept(), evl_net_ether_accept_vlan())
 * diverting packets from the regular networking stack, in order to
 * queue work for an oob protocol handler.
 *
 * We may be either called in-band, or out-of-band on behalf of a
 * fully oob-capable NIC driver, typically from a NAPI poll handler
 * running out-of-band.
 *
 * @skb the packet to queue. skb->dev must be valid.
 *
 * @handler the network protocol descriptor which should eventually
 * handle the packet.
 */
void evl_net_receive(struct sk_buff *skb,
		struct evl_net_handler *handler) /* in-band or oob */
{
	struct net_device *dev = skb->dev;
	struct evl_netdev_state *est = dev->oob_state.estate;

	if (EVL_WARN_ON(NET, dev == NULL))
		return;

	if (refcount_read(&evl_net_rx_timestamping) > 1) {
		skb_shinfo_oob(skb)->device_time = evl_ktime_monotonic();
		skb_mark_oob_timestamped(skb);
	}

	EVL_NET_CB(skb)->handler = handler;

	skb_reset_network_header(skb);
	if (!skb_transport_header_was_set(skb))
		skb_reset_transport_header(skb);
	skb_reset_mac_len(skb);

	/*
	 * Enqueue the packet. The NIC driver is expected to call
	 * napi_complete_done() when the RX side goes quiescent, which
	 * will in turn wake up our RX thread via a call to
	 * evl_net_wake_rx(). Ancient non-NAPI drivers would have to
	 * call evl_net_wake_rx() explicitly whenever they see fit.
	 */
	evl_net_add_skb_queue(&est->rx_packets, skb);
}

/**
 * napi_schedule_oob - plan for polling a NAPI instance.
 *
 * The RX kthread is resumed so that it polls the associated device
 * for ingress packets directly from the oob stage.
 *
 * This routine is usually called out-of-band in order to schedule the
 * RX thread for handling the packets accepted by netif_deliver_oob()
 * directly from the oob stage. When called in-band though, the RX
 * thread is resumed to process the packets received (in-band) from a
 * non oob-capable device diverting traffic to EVL, so that it passes
 * those packets to the proper protocol handlers in our netstack.
 *
 * @n is the NAPI instance associated to a device for which oob packet
 * diversion is enabled. An earlier call to napi_schedule_prep() is
 * expected to have been issued for @n.
 */
void napi_schedule_oob(struct napi_struct *n) /* inband/oob */
{
	struct net_device *dev = n->dev;
	struct evl_netdev_state *est = dev->oob_state.estate;
	unsigned long flags;

	if (EVL_WARN_ON(NET, !(n->state & NAPIF_STATE_SCHED)))
		return;

	/*
	 * We might have multiple NAPI instances per device, so
	 * serialization is required despite a single NAPI instance
	 * may be active at any point in time. Oh, well. See do_poll()
	 * for an explanation about the requirement for atomic bitops
	 * (EVL_NETDEV_RX_SCHED_BIT).
	 */
	if (likely(running_oob())) {
		raw_spin_lock_irqsave(&est->napi_lock, flags);
		list_add(&n->poll_list, &est->napi_poll);
		set_bit(EVL_NETDEV_RX_SCHED_BIT, &est->flags);
		raw_spin_unlock_irqrestore(&est->napi_lock, flags);
		wake_rx(est);
	} else {
		evl_net_wake_rx(dev);
	}
}

/**
 * netif_deliver_oob - receive a network packet from the hardware.
 *
 * Decide whether we should channel a freshly incoming packet to our
 * out-of-band stack. May be called from any stage.
 *
 * Delivery is trivially simple at the moment: the set of protocols we
 * handle is statically defined, currently ETH_P_IP. The point is to
 * provide an expedited data path via the oob stage for the protocols
 * which most users need, without reinventing the whole networking
 * infrastructure.
 *
 * @skb the packet to inspect for oob delivery. May be linked to some
 * upstream queue.
 *
 * Returns true if the oob stack wants to handle @skb, in which case
 * the caller must assume that it does not own the packet anymore.
 */
bool netif_deliver_oob(struct sk_buff *skb) /* oob or in-band */
{
	struct net_device *dev = skb->dev;
	bool picked;

	/* We deal with Ethernet and loopback devices only. */
	if (unlikely(dev->type != ARPHRD_ETHER && dev->type != ARPHRD_LOOPBACK))
		return false;

	/*
	 * Filter the incoming packet through the eBPF RX program
	 * attached to the input device (if any), passing it down to
	 * the regular in-band stack if the filter code says that we
	 * are not interested in it. If no filter is active,
	 * EVL_RX_VLAN is applied.
	 */
	switch (evl_net_filter_rx(dev, skb)) {
	case EVL_RX_VLAN:
		picked = evl_net_ether_accept_vlan(skb);
		/* Apply our regular VLAN-based filter. */
		break;
	case EVL_RX_ACCEPT:
		/* Try accepting the packet regardless of VLAN tagging. */
		picked = evl_net_ether_accept(skb);
		break;
	case EVL_RX_DROP:
		/* Blackhole. */
		evl_net_free_skb(skb);
		return true;
	case EVL_RX_SKIP:
	default:
		/* Leave the packet to inband. */
		return false;
	}

	/*
	 * Feed in-band input taps if any. Racing with in-band updates
	 * to the packet type chain is ok, we don't dereference it but
	 * only use a hint to determine whether we should push the
	 * buffer to the in-band nit, all operations are properly
	 * serialized there.
	 */
	if (picked && dev_nit_active(dev))
		evl_net_tap_in(dev, skb);

	return picked;
}

/*
 * The manual rescheduling call for legacy NIC drivers which are not
 * NAPI-compliant, but would rather use the netif_rx() interface
 * instead.
 */
void netif_schedule_oob(struct net_device *dev)
{
	evl_net_wake_rx(dev);
}
EXPORT_SYMBOL(netif_schedule_oob);
