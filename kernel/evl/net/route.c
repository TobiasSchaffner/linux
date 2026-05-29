/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2023 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/slab.h>
#include <linux/inetdevice.h>
#include <net/route.h>
#include <evl/net/socket.h>
#include <evl/net/ipv4.h>
#include <evl/net/ipv4/route.h>
#include <evl/net/ipv4/arp.h>

/*
 * in-band hook which receives IPv4 routing decisions which go through
 * an oob-enabled device.
 */
void ip_learn_oob_route(struct net *net, struct flowi4 *fl4, struct rtable *rt)
{
	evl_net_learn_ipv4_route(net, fl4, rt);
}

/*
 * Disconnect a device from the routing system by purging the route
 * and neighbour caches from entries associated to this device.
 */
void evl_net_del_route_dev(struct net_device *dev)
{
  	if  (!EVL_WARN_ON(NET, !netif_oob_port(dev)))
		return;

	if (rcu_access_pointer(dev->ip_ptr)) {
		evl_net_ipv4_purge_dev(dev_net(dev), dev);
		evl_net_flush_arp(dev_net(dev), dev);
	}
}

/*
 * Purge the route cache from entries referring to the given address
 * as source.
 */
void evl_net_del_route_src(struct net_device *dev, struct in_ifaddr *ifa)
{
	if (rcu_access_pointer(dev->ip_ptr))
		evl_net_ipv4_purge_src(dev_net(dev), ifa);
}
