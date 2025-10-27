/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2021-2023 Philippe Gerum  <rpm@xenomai.org>
 *
 * This file implements a simple out-of-band front cache to the ARP
 * table maintained by the in-band network stack. Basically, we listen
 * to update events from the later, caching new complete entries
 * observed on oob-enabled devices, uncaching invalidated/dead
 * entries. The front cache may be used safely from oob context for
 * address lookup.
 */

#include <linux/if_ether.h>
#include <linux/hash.h>
#include <linux/notifier.h>
#include <linux/wait.h>
#include <linux/etherdevice.h>
#include <net/netevent.h>
#include <net/arp.h>
#include <evl/net/ipv4/arp.h>

DECLARE_WAIT_QUEUE_HEAD(evl_arp_event);

#define EVL_NET_ARP_CACHE_SHIFT  8

static struct evl_net_arp_entry *alloc_arp_entry(
	struct net_device *dev,
	__be32 addr, unsigned char *ha) /* in-band */
{
	struct evl_net_arp_entry *e;

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return NULL;

	e->key.dev = dev;
	e->key.addr = addr;
	if (ha)
		memcpy(e->ha, ha, sizeof(e->ha));

	netdev_hold(dev, &e->dev_tracker, GFP_ATOMIC);

	return e;
}

static void free_arp_entry(struct evl_net_arp_entry *e)
{
	netdev_put(e->key.dev, &e->dev_tracker);
	kfree(e);
}

static u32 hash_arp_entry(const void *key)
{
	const struct evl_net_arp_key *arp_k = key;
	return arp_k->addr ^ hash32_ptr(arp_k->dev);
}

static bool eq_arp_entry(const struct evl_cache_entry *entry,
			const void *key)
{
	const struct evl_net_arp_entry *e =
		container_of(entry, struct evl_net_arp_entry, entry);
	const struct evl_net_arp_key *arp_k = key;

	return e->key.addr == arp_k->addr && e->key.dev == arp_k->dev;
}

static char *format_arp_key(const struct evl_cache_entry *entry)
{
	const struct evl_net_arp_entry *e =
		container_of(entry, struct evl_net_arp_entry, entry);

	return kasprintf(GFP_ATOMIC, "%pI4", &e->key.addr);
}

static const void *get_arp_key(const struct evl_cache_entry *entry)
{
	const struct evl_net_arp_entry *e =
		container_of(entry, struct evl_net_arp_entry, entry);

	return &e->key;
}

static void drop_arp_entry(struct evl_cache_entry *entry) /* in-band */
{
	struct evl_net_arp_entry *e =
		container_of(entry, struct evl_net_arp_entry, entry);

	free_arp_entry(e);
}

static struct evl_cache_ops arp_cache_ops = {
	.hash		= hash_arp_entry,
	.eq		= eq_arp_entry,
	.get_key	= get_arp_key,
	.format_key	= format_arp_key,
	.drop		= drop_arp_entry,
};

/*
 * Cache a new ARP entry. A previous match is replaced by the new one.
 */
static int cache_arp_entry(struct evl_cache *cache, struct neighbour *neigh) /* in-band */
{
	__be32 addr = *(const __be32 *)neigh->primary_key;
	struct evl_net_arp_entry *e;
	int ret;

	e = alloc_arp_entry(neigh->dev, addr, neigh->ha);
	if (!e)
		return -ENOMEM;

	ret = evl_add_cache_entry(cache, &e->entry);
	if (ret)
		free_arp_entry(e);

	return ret;
}

/*
 * Uncache an ARP entry.
 */
static void uncache_arp_entry(struct evl_cache *cache, struct neighbour *neigh) /* in-band */
{
	const struct evl_net_arp_key key = {
		.addr = *(const __be32 *)neigh->primary_key,
		.dev = neigh->dev,
	};

	evl_del_cache_entry(cache, &key);
}

/*
 * Handle an update notification from the in-band ARP cache.
 */
static void update_arp_cache(struct neighbour *neigh) /* in-band */
{
	struct net_device *dev = neigh->dev;
	struct oob_net_state *nets = &dev_net(dev)->oob;
	struct evl_cache *cache = &nets->ipv4.arp;
	int ret;

	read_lock_bh(&neigh->lock); /* Protect against races on nud_state */

	if (netif_oob_port(dev))
		netdev_dbg(dev, "proto=%#x, state=%#x, dead=%d, "
			"iface=%s, ip=%pI4, mac=%pM\n",
			ntohs(neigh->tbl->protocol), neigh->nud_state,
			neigh->dead, netdev_name(neigh->dev),
			neigh->primary_key, neigh->ha);

	if (neigh->nud_state & NUD_REACHABLE && netif_oob_port(dev)) {
		/* Cache complete entries from oob-enabled devices. */
		ret = cache_arp_entry(cache, neigh);
		if (ret)
			/*
			 * Yeah, well. Nothing useful we can do. Now
			 * the oob cache is out of sync and since the
			 * in-band one is up to date, nothing would
			 * justify to redo the resolution so that we
			 * might catch its output - except flushing
			 * the in-band cache entirely. This said, this
			 * would likely be the gentlest effect of
			 * receiving OOM here anyway.
			 */
			printk(EVL_WARNING "out of memory for ARP cache\n");
		else
			wake_up_all(&evl_arp_event);
	} else {
		/*
		 * Try uncaching any invalidated, stale or dead entry.
		 * Out-of-band caps might have just been turned off
		 * for the device although we still keep entries
		 * referring to it, so do not filter out on
		 * netif_oob_port().
		 */
		if (neigh->dead || neigh->nud_state & (NUD_FAILED|NUD_STALE))
			uncache_arp_entry(cache, neigh);
	}

	read_unlock_bh(&neigh->lock);
}

static int netevent_handler(struct notifier_block *nb,
			unsigned long event, void *arg)
{
 	struct neighbour *neigh = arg;

	if (event == NETEVENT_NEIGH_UPDATE && neigh->tbl == &arp_tbl)
		update_arp_cache(neigh);

	return NOTIFY_DONE;
}

struct evl_net_arp_entry *evl_net_get_arp_entry(struct net_device *dev, __be32 addr)
{
	const struct evl_net_arp_key key = {
		.addr = addr,
		.dev = dev,
	};
	struct oob_net_state *nets = &dev_net(dev)->oob;
	struct evl_cache_entry *entry;

	entry = evl_lookup_cache(&nets->ipv4.arp, &key);
	if (likely(entry))
		return container_of(entry, struct evl_net_arp_entry, entry);

	return NULL;
}

/*
 * Add a new neighbour to our ARP front cache. Any previous match is
 * replaced by the new one.
 */
int evl_net_update_arp(struct neighbour *neigh) /* inband */
{
	struct oob_net_state *nets = &dev_net(neigh->dev)->oob;
	struct evl_cache *cache = &nets->ipv4.arp;
	int ret = -ESTALE;

	read_lock_bh(&neigh->lock);

	/*
	 * We only cache entries for connected neighbours: recheck
	 * under lock.
	 */
	if (READ_ONCE(neigh->nud_state) & NUD_CONNECTED)
		ret = cache_arp_entry(cache, neigh);

	read_unlock_bh(&neigh->lock);

	return ret;
}

static struct evl_net_arp_entry *
fill_pseudo_arp(struct net_device *dev, __be32 ipaddr,
		const u8 *hwaddr, int hwlen,
		struct evl_net_arp_entry *pseudo_earp)
{
	if (hwaddr)
		memcpy(pseudo_earp->ha, hwaddr, hwlen);
	else
		memset(pseudo_earp->ha, 0, hwlen);

	pseudo_earp->key.dev = dev;
	pseudo_earp->key.addr = ipaddr;

	return pseudo_earp;
}

struct evl_net_arp_entry *
evl_net_get_arp_entry_or_pseudo(struct net_device *dev, __be32 ipaddr,
				struct evl_net_arp_entry *pseudo_earp)
{
	/*
	 * We don't grab any reference on the device for which a
	 * pseudo-ARP entry is resolved, the caller is supposed to
	 * have one already, and expected not to put back this
	 * temporary entry.
	 */
	if (unlikely(ipv4_is_loopback(ipaddr)))
		return fill_pseudo_arp(dev, ipaddr,
				NULL, ETH_ALEN, pseudo_earp);

	if (unlikely(ipv4_is_lbcast(ipaddr)))
		return fill_pseudo_arp(dev, ipaddr,
				dev->broadcast, ETH_ALEN, pseudo_earp);

	if (ipv4_is_multicast(ipaddr))
		return fill_pseudo_arp(dev, ipaddr, eth_ipv4_mcast_addr_base,
				sizeof(eth_ipv4_mcast_addr_base), pseudo_earp);

	/* Not a pseudo-ARP, look up into the cache. */
	return evl_net_get_arp_entry(dev, ipaddr);
}

static struct notifier_block netevent_notifier __read_mostly = {
	.notifier_call = netevent_handler,
};

static bool compare_arp_dev(struct evl_cache_entry *entry, void *arg)
{
	const struct evl_net_arp_entry *e =
		container_of(entry, struct evl_net_arp_entry, entry);
	struct net_device *dev = arg;

	return dev == e->key.dev;
}

/*
 * Flush the ARP entries maintained in the out-of-band front cache. If
 * @dev is non-NULL, only the entries associated to the device are
 * dropped. Otherwise, NULL is a wildcard for purging the cache
 * entirely.
 */
void evl_net_flush_arp(struct net *net, struct net_device *dev)
{
	struct oob_net_state *nets = &net->oob;

	if (dev)
		evl_clean_cache(&nets->ipv4.arp, compare_arp_dev, dev);
	else
		evl_flush_cache(&nets->ipv4.arp);
}

int evl_net_init_arp(struct net *net)
{
	struct oob_net_state *nets = &net->oob;
	struct evl_cache *cache;
	int ret;

	/* ARP resolution cache. */
	cache = &nets->ipv4.arp;
	cache->ops = &arp_cache_ops;
	cache->init_shift = EVL_NET_ARP_CACHE_SHIFT;
	cache->name = "ARP";

	ret = evl_init_cache(cache);
	if (ret)
		return ret;

	register_netevent_notifier(&netevent_notifier);

	return 0;
}

void evl_net_cleanup_arp(struct net *net)
{
	unregister_netevent_notifier(&netevent_notifier);
	evl_net_flush_arp(net, NULL);
}
