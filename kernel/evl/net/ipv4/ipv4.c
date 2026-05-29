/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2023 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/net.h>
#include <linux/in.h>
#include <linux/units.h>
#include <linux/jhash.h>
#include <linux/notifier.h>
#include <linux/inetdevice.h>
#include <net/ip.h>
#include <net/netevent.h>
#include <net/neighbour.h>
#include <evl/assert.h>
#include <evl/mutex.h>
#include <evl/memory.h>
#include <evl/net/socket.h>
#include <evl/net/skb.h>
#include <evl/net/device.h>
#include <evl/net/ipv4.h>
#include <evl/net/ipv4/input.h>
#include <evl/net/ipv4/route.h>
#include <evl/net/ipv4/arp.h>
#include <evl/net/ipv4/udp.h>
#include <evl/net/ipv4/icmp.h>

int evl_net_ipv4_solicit_timeout = 5; /* Seconds */

/*
 * Setup the IPv4 portion of the EVL state into an in-band network
 * namespace (struct net { ... struct oob_net_state oob; ... }).
 */
int evl_net_init_ipv4(struct net *net)
{
	struct oob_net_state *nets = &net->oob;
	struct evl_net_frag_tdir *ftdir;
	struct evl_net_frag_gc *gc;
	int ret;

	ret = evl_net_init_ipv4_routing(net);
	if (ret)
		return ret;

	ret = evl_net_init_arp(net);
	if (ret)
		goto fail_arp;

	ret = evl_net_init_udp(net);
	if (ret)
		goto fail_udp;

	ret = evl_net_init_icmp(net);
	if (ret)
		goto fail_icmp;

	/* Fragment directory and friends. */
	ftdir = &nets->ipv4.ftdir;
	hash_init(ftdir->ht);
	evl_init_kmutex(&ftdir->lock);
	ftdir->timeout = (ktime_t)IP_FRAG_TIME / HZ * NANOHZ_PER_HZ;
	gc = &ftdir->gc;
	INIT_HLIST_HEAD(&gc->queue);
	raw_spin_lock_init(&gc->lock);
	might_hard_lock(&gc->lock);

	return 0;

fail_icmp:
	evl_net_cleanup_udp(net);
fail_udp:
	evl_net_cleanup_arp(net);
fail_arp:
	evl_net_cleanup_ipv4_routing(net);

	return ret;
}

void evl_net_cleanup_ipv4(struct net *net)
{
	struct oob_net_state *nets = &net->oob;
	struct evl_net_frag_gc *gc = &nets->ipv4.ftdir.gc;

	evl_net_cleanup_udp(net);
	evl_net_cleanup_arp(net);
	evl_net_cleanup_ipv4_routing(net);
	EVL_WARN_ON(NET, !hlist_empty(&gc->queue));
}

int evl_net_ipv4_handle_event(struct notifier_block *nb,
			unsigned long event, void *arg)
{
 	struct neighbour *neigh = arg;

	if (event == NETEVENT_NEIGH_UPDATE && neigh->tbl == &arp_tbl)
		evl_net_arp_update_cache(neigh);

	return NOTIFY_DONE;
}

/*
 * Check whether a connected neighbour is available from our front
 * cache, otherwise solicit the peer who should respond to the ARP
 * request within the allotted time. In the latter case, probing is
 * forced for stale entries.
 */
static int check_probe_neighbour(struct neighbour *neigh)
{
	u8 nud_state = READ_ONCE(neigh->nud_state);
	int ret;

	if (EVL_WARN_ON(NET, nud_state & NUD_NOARP))
		return -EINVAL;

	if (nud_state & NUD_CONNECTED) {
		ret = evl_net_update_arp(neigh);
		if (ret != -ESTALE)
			return ret;
		nud_state = READ_ONCE(neigh->nud_state);
	}

	if (nud_state & NUD_STALE) {
		/* Invalidate/expire a stale entry to force probing. */
		ret = neigh_update(neigh, NULL, NUD_NONE,
				NEIGH_UPDATE_F_OVERRIDE|
				NEIGH_UPDATE_F_ADMIN, 0);
		if (ret)
			return ret;
	}

	/* Start the probing process. */
	neigh_event_send(neigh, NULL);

	return 0;
}

/**
 * evl_net_ipv4_solicit - Resolve an IPv4 address into a link-layer
 * address using ARP neighbour solicitation. This call waits for the
 * number of seconds read from evl_net_ipv4_solicit_timeout for the
 * front cache to receive the ARP entry before timing out.
 *
 * @net		Network namespace this operation applies to.
 * @dev		Preferred output device, or NULL if unspec. If given, @dev
 *		must be enabled as an oob port.
 * @addr	IPv4 address of the peer to solicit.
 * @flags	Operation flags. EVL_NEIGH_PERMANENT locks the ARP entry
 *		in the in-band cache. EVL_NEIGH_MAYROUTE allows gateways
 *              on the path to destination.
 *
 * Returns zero on success.
 */
int evl_net_ipv4_solicit(struct net *net,
			struct net_device *dev,
			struct sockaddr_unsized *addr, int flags) /* inband */
{
	struct evl_net_arp_entry *e = NULL;
	struct neighbour *neigh;
	struct rtable *rt;
	__be32 ipaddr;
	long ret = 0;
	u8 scope;

	if (addr->sa_family != AF_INET)
		return -EAFNOSUPPORT;

	if (flags & ~(EVL_NEIGH_PERMANENT|EVL_NEIGH_MAYROUTE))
		return -EINVAL;

	if (dev && !netif_oob_port(dev))
		return -ENODEV;

	ipaddr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;

	/*
	 * Tell the in-band stack to resolve the route to the peer. On
	 * success, we are called back via ip_learn_oob_route() with
	 * the routing data, which we then index into our oob route
	 * cache.
	 */
	scope = flags & EVL_NEIGH_MAYROUTE ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
	rt = ip_route_output(net, ipaddr, INADDR_ANY, 0,
			     dev ? dev->ifindex : 0, scope);
	if (IS_ERR(rt))
		return PTR_ERR(rt);

	dev = rt->dst.dev;
	if (!dev || !netif_oob_port(dev))
		return -ENODEV;

	evl_net_get_dev(dev);

	neigh = dst_neigh_lookup(&rt->dst, &ipaddr);
	ip_rt_put(rt);
	if (!neigh) {
		evl_net_put_dev(dev);
		return -ENOENT;
	}

	if (likely(!(neigh->nud_state & NUD_NOARP))) {
		ret = check_probe_neighbour(neigh);
		if (ret)
			goto out;
		ret = wait_event_interruptible_timeout(evl_arp_event,
						(e = evl_net_get_arp_entry(dev, ipaddr)),
						evl_net_ipv4_solicit_timeout * HZ
			);
		ret = e ? 0 : ret ?: -ETIMEDOUT;
	} else {
		ret = evl_net_update_arp(neigh);
		if (ret)
			return ret;
		e = evl_net_get_arp_entry(dev, ipaddr);
	}
out:
	evl_net_put_dev(dev);

	if (e) {
		/* We never downgrade the permanent state. */
		if (flags & EVL_NEIGH_PERMANENT &&
			!(neigh->nud_state & NUD_PERMANENT))
			ret = neigh_update(neigh, e->ha, NUD_PERMANENT,
				NEIGH_UPDATE_F_OVERRIDE | NEIGH_UPDATE_F_ADMIN, 0);
		evl_net_put_arp_entry(e);
	}

	neigh_release(neigh);

	return ret;
}

/**
 *	evl_net_ipv4_devaddr - Get the IPv4 address of a device.
 *
 *	The caller must hold the RCU lock.
 */
__be32 evl_net_ipv4_devaddr(const struct net_device *dev)
{
	struct in_device *in_dev = __in_dev_get_rcu(dev);
	const struct in_ifaddr *ifa;

	if (EVL_WARN_ON(EVL, !in_dev)) /* Ummh, oh.. */
		return 0;

	ifa = rcu_dereference(in_dev->ifa_list);
	if (EVL_WARN_ON(EVL, !ifa)) /* Seriously? */
		return 0;

	return ifa->ifa_address;
}

static struct evl_net_proto *match_ipv4_domain(int type, int protocol)
{
	switch (protocol) {
	case IPPROTO_UDP:
		if (type != SOCK_DGRAM)
			return ERR_PTR(-ESOCKTNOSUPPORT);

		return &evl_net_udp_proto;
	case IPPROTO_ICMP:
		if (type != SOCK_DGRAM)
			return ERR_PTR(-ESOCKTNOSUPPORT);

		return &evl_net_icmp_proto;
	default:
		return NULL;
	}
}

struct evl_socket_domain evl_net_ipv4 = {
	.af_domain = AF_INET,
	.match = match_ipv4_domain,
};
