/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_DEVICE_H
#define _EVL_NET_DEVICE_H

#ifdef CONFIG_EVL_NET

#include <linux/if_vlan.h>
#include <uapi/evl/net/device-abi.h>
#include <uapi/evl/net/bpf-abi.h>

struct evl_net_devparams;
struct notifier_block;
struct sk_buff;

int evl_net_switch_oob_port(struct net_device *dev,
			    struct evl_net_devparams *p);

int evl_netdev_event(struct notifier_block *ev_block,
		     unsigned long event, void *ptr);

struct net_device *
evl_net_get_dev_by_index(struct net *net, int ifindex);

struct net_device *
evl_net_get_dev_by_flags(struct net *net, int flags);

struct net_device *
evl_net_get_dev_by_name(struct net *net, const char *name);

void evl_net_get_dev(struct net_device *dev);

void evl_net_put_dev(struct net_device *dev);

struct net_device *
evl_net_find_vlan_dev(struct net *net,
		__be16 vlan_proto, __u16 vlan_id);

void evl_net_wake_rx(struct net_device *dev);

int __evl_net_dev_allocfd(struct net_device *dev);

int evl_net_dev_allocfd(struct net *net, const char *devname);

void evl_net_dev_tx_nomem(struct net_device *dev);

static inline struct net_device *evl_net_real_dev(struct net_device *dev)
{
	if (is_vlan_dev(dev))
		return vlan_dev_real_dev(dev);

	return dev;
}

static inline struct evl_netdev_state *evl_net_get_state(struct net_device *dev)
{
	struct net_device *real_dev = evl_net_real_dev(dev);

	return real_dev->oob_state.estate;
}

static inline struct evl_netdev_stats *evl_net_get_stats(struct net_device *dev)
{
	return dev->oob_state.stats;
}

enum evl_net_rx_action
__evl_net_filter_rx(struct evl_netdev_state *est, struct sk_buff *skb);

static inline enum evl_net_rx_action
evl_net_filter_rx(struct net_device *dev, struct sk_buff *skb)
{
	struct evl_netdev_state *est = evl_net_get_state(dev);

	/* We should receive traffic only from base/physical interfaces. */
	if (EVL_WARN_ON_ONCE(NET, is_vlan_dev(dev)))
		return EVL_RX_SKIP;

	if (test_bit(EVL_NETDEV_RX_FILTER_BIT, &est->flags))
		return __evl_net_filter_rx(est, skb);

	/*
	 * If no filter handled the packet and the oob port is enabled
	 * directly on the receiving (base) interface, then assume
	 * that such device is entirely dedicated to oob
	 * traffic. Accept all packets flowing in.  This includes
	 * IFF_LOOPBACK devices (e.e. 'lo') so that we unconditionally
	 * accept incoming traffic if we have an active oob port
	 * there.
	 */
	return netif_oob_port(dev) ? EVL_RX_ACCEPT : EVL_RX_VLAN;
}

#endif

#endif /* !_EVL_NET_DEVICE_H */
