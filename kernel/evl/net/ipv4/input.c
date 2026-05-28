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

/*
 * Fragment expiration handler. Triggers when a fragmented datagram
 * could not be reassembled within the allotted time (IP_FRAG_TIME).
 */
static void frag_expired(struct evl_timer *timer)
{
	struct evl_net_frag_tdir *ftdir;
	struct evl_net_frag_tree *ft;

	ft = container_of(timer, struct evl_net_frag_tree, timer);
	ftdir = ft->tdir;
	raw_spin_lock(&ftdir->gc.lock);
	hlist_add_head(&ft->gc, &ftdir->gc.queue);
	raw_spin_unlock(&ftdir->gc.lock);
	/* Tell the RX thread to run the garbage collection. */
	evl_net_wake_rx(ft->gc_dev);
	netdev_warn(ft->gc_dev, "reassembly timed out, frag tree %px\n", ft);
}

static u32 hash_frag_key(const void *data, u32 len)
{
	return jhash2(data, len / sizeof(u32), 0);
}

static bool compare_frag_key(struct evl_net_frag_tree *ft,
			const struct frag_v4_compare_key *key)
{
	return !memcmp(&ft->key.ipv4, key, sizeof(*key));
}

static struct evl_net_frag_tree *
alloc_frag_tree(struct evl_net_frag_tdir *ftdir,
		const struct frag_v4_compare_key *key,
		struct net_device *dev)
{
	struct evl_net_frag_tree *ft;

	ft = evl_alloc(sizeof(*ft));
	if (!ft)
		return NULL;

	ft->end = 0;
	ft->len = 0;
	ft->flags = 0;
	ft->frags = RB_ROOT;
	ft->tdir = ftdir;
	ft->gc_dev = dev;
	INIT_HLIST_NODE(&ft->gc);
	ft->key.ipv4 = *key;
	evl_init_timer(&ft->timer, &evl_mono_clock, frag_expired);
	evl_spin_lock_init(&ft->lock);
	netdev_dbg(dev, "allocated frag tree %px\n", ft);

	return ft;
}

/* ftdir->lock held. */
static struct evl_net_frag_tree *
get_frag_tree(struct evl_net_frag_tdir *ftdir,
	const struct frag_v4_compare_key *key,
	struct net_device *dev)
{
	u32 hashval = hash_frag_key(key, sizeof(*key));
	struct evl_net_frag_tree *ft;

	hash_for_each_possible(ftdir->ht, ft, hash, hashval) {
		if (compare_frag_key(ft, key))
			return ft;
	}

	ft = alloc_frag_tree(ftdir, key, dev);
	if (!ft)
		return ERR_PTR(-ENOMEM);

	hash_add(ftdir->ht, &ft->hash, hashval);
	netdev_dbg(dev, "hashed frag tree %px\n", ft);

	/* Starts aging only once hashed. */
	evl_start_timer(&ft->timer,
			evl_abs_timeout(&ft->timer, ftdir->timeout),
			0);
	return ft;
}

/*
 * Index the new fragment into the frag tree on the fragment offset
 * found into the IP header.
 *
 * CAUTION: since ->rbnode and ->dev are unionized in sk_buff, the
 * device the indexed skbs came from can only be found in the heading
 * skb holding them (which is not indexed).
 *
 * @offset is a count of 8-byte chunks.
 *
 * ftdir->lock and ft->lock held, irqs off.
 */
static int index_frag(struct evl_net_frag_tree *ft, int offset, struct sk_buff *skb)
{
	struct rb_node **rbp, *parent;
	int ret = 0;

	parent = NULL;
	rbp = &ft->frags.rb_node;

	while (*rbp) {
		struct sk_buff *e = rb_entry(*rbp, struct sk_buff, rbnode);
		struct iphdr *iph = ip_hdr(e);
		int _offset = ntohs(iph->frag_off) & IP_OFFSET;
		parent = *rbp;
		if (offset < _offset)
			rbp = &(*rbp)->rb_left;
		else if (offset > _offset)
			rbp = &(*rbp)->rb_right;
		else
			return -EEXIST; /* Duplicate - drop it. */
	}

	rb_link_node(&skb->rbnode, parent, rbp);
	rb_insert_color(&skb->rbnode, &ft->frags);

	return ret;
}

/*
 * Reassemble the datagram, connecting all skbs indexed in the frag
 * tree as a single-linked list in logical offset order. The tree is
 * guaranteed non-empty on entry.
 *
 * @ft  the frag tree to reassemble from.
 *
 * No lock held, the frag tree is not hashed, only known to the
 * caller.
 */
static struct sk_buff *reasm_frag(struct net *net,
				struct evl_net_frag_tree *ft,
				struct net_device *dev)
{
	struct rb_node *rb = rb_first(&ft->frags);
	struct sk_buff *head, **skbp, *fskb;

	/* This has to be the heading packet at offset 0. */
	head = rb_entry(rb, struct sk_buff, rbnode);
	head->dev = dev;
	skbp = &skb_shinfo(head)->frag_list;
	rb = rb_next(rb);

	while (rb) {
		fskb = rb_entry(rb, struct sk_buff, rbnode);
		fskb->dev = dev;
		*skbp = fskb;
		skbp = &fskb->next;
		rb = rb_next(rb);
	}

	*skbp = NULL;

	return head;
}

/*
 * Push an incoming fragment to the corresponding frag tree. The main
 * logic was shamelessly lifted from ip_frag_queue().
 */
static struct sk_buff *push_frag(struct sk_buff *skb, struct net_device *dev)
{
	struct net *net = dev_net(dev);
	struct evl_net_frag_tdir *ftdir = &net->oob.ipv4.ftdir;
	struct iphdr *iph = ip_hdr(skb);
	struct frag_v4_compare_key key = {
		.saddr = iph->saddr,
		.daddr = iph->daddr,
		.user = IP_DEFRAG_LOCAL_DELIVER,
		.id = iph->id,
		.protocol = iph->protocol,
	};
	int offset, floff, end, len, ret;
	struct evl_net_frag_tree *ft;
	struct sk_buff *head;
	unsigned long flags;

	rcu_read_lock();
	key.vif = l3mdev_master_ifindex_rcu(dev);
	rcu_read_unlock();

	evl_lock_kmutex(&ftdir->lock);

	ft = get_frag_tree(ftdir, &key, dev);
	if (IS_ERR(ft)) {
		ret = PTR_ERR(ft);
		goto out_notree;
	}

	evl_spin_lock(&ft->lock);

	floff = ntohs(iph->frag_off);
	offset = (floff & IP_OFFSET) << 3; /* 8-byte chunks */
	/*
	 * The logical end offset of the packet, stripping out the
	 * l2+IP headers.
	 */
	end = offset + skb->len - skb_network_offset(skb) - ip_hdrlen(skb);

	netdev_dbg(dev, "pushing id=%d to frag tree %px, frag_off=%#x, ipfl=%#x, ihl=%d, %pI4 -> %pI4\n",
		iph->id, ft, ntohs(iph->frag_off), floff & ~IP_OFFSET,
		ip_hdrlen(skb), &iph->saddr, &iph->daddr);

	ret = -EINVAL;
	if (!(floff & IP_MF)) {	/* Last fragment in the series? */
		/*
		 * If we were already past the incoming fragment, or
		 * received a different end, this fragment is
		 * corrupted, so we reject it.
		 */
		if (end < ft->end ||
			((ft->flags & INET_FRAG_LAST_IN) && end != ft->end))
			goto out;

		ft->flags |= INET_FRAG_LAST_IN;
		ft->end = end;
		netdev_dbg(dev, "final frag id=%d\n", iph->id);
	} else {
		netdev_dbg(dev, "more frag(s) id=%d\n", iph->id);
		/*
		 * Inner frag length should be a multiple of 8
		 * bytes.
		 */
		if (end & 7) {
			end &= ~7;
			if (skb->ip_summed != CHECKSUM_UNNECESSARY)
				skb->ip_summed = CHECKSUM_NONE;
		}
		if (end > ft->end) {
			/*
			 * If receiving data beyond the final packet,
			 * the incoming packet is corrupt.
			 */
			if (ft->flags & INET_FRAG_LAST_IN)
				goto out;

			ft->end = end;
		}
	}

	if (end == offset)	/* Zero-sized? Ignore then. */
		goto out;

	if (offset == 0) {
		ft->flags |= INET_FRAG_FIRST_IN;
		netdev_dbg(dev, "first frag id=%d\n", iph->id);
	}

	/* Update the logical length received (headers stripped). */
	ft->len += end - offset;

	netdev_dbg(dev, "indexing frag id=%d\n", iph->id);
	ret = index_frag(ft, offset >> 3, skb);
	if (ret)
		goto out;

	/* If complete, reassemble the datagram. */
	if (ft->flags == (INET_FRAG_FIRST_IN | INET_FRAG_LAST_IN) &&
		ft->len == ft->end) {
		netdev_dbg(dev, "completed frag id=%d, len=%zu\n", iph->id, ft->len);
		evl_spin_unlock(&ft->lock);
		/*
		 * Stop the timer, then move the frag tree out of the
		 * gc queue if it's linked there.
		 */
		evl_stop_timer(&ft->timer);
		raw_spin_lock_irqsave(&ftdir->gc.lock, flags);
		if (!hlist_unhashed(&ft->gc))
			hlist_del(&ft->gc);
		raw_spin_unlock_irqrestore(&ftdir->gc.lock, flags);
		/* Covered by ftdir->lock. */
		hlist_del(&ft->hash);
		evl_unlock_kmutex(&ftdir->lock);
		head = reasm_frag(net, ft, dev);
		len = ip_hdrlen(skb) + ft->len;
		evl_free(ft);
		if (len > 65535) { /* RFC 791 */
			evl_net_free_skb(head); /* Timer is off, so we have to cleanup manually. */
			return ERR_PTR(-E2BIG);
		}
		return head;
	}

	/* Tell the caller to wait for more. */
	ret = -EINPROGRESS;
out:
	evl_spin_unlock(&ft->lock);
out_notree:
	evl_unlock_kmutex(&ftdir->lock);

	return ERR_PTR(ret);
}

/*
 * Run the garbage collection for a given net, like dropping outdated
 * IPv4 frags.
 */
void __evl_net_ipv4_gc(struct evl_net_frag_tdir *ftdir)
{
	struct evl_net_frag_tree *ft;
	struct hlist_head tmp;
	struct hlist_node *n;
	unsigned long flags;

	raw_spin_lock_irqsave(&ftdir->gc.lock, flags);
	hlist_move_list(&ftdir->gc.queue, &tmp);
	raw_spin_unlock_irqrestore(&ftdir->gc.lock, flags);

	evl_lock_kmutex(&ftdir->lock);

	hlist_for_each_entry_safe(ft, n, &tmp, gc) {
		netdev_dbg(ft->gc_dev, "free frag tree %px\n", ft);
		hlist_del(&ft->hash);
		evl_free(ft);
	}

	evl_unlock_kmutex(&ftdir->lock);
}

/*
 * evl_net_ipv4_deliver - deliver an IPv4 packet to its final handler
 * (typically the UDP layer).
 *
 * @skb the packet to deliver to the IPv4 stack.
 *
 * On error from this routine, the caller should care of dropping
 * @skb.
 *
 * The logic of this code borrows a lot from ip_rcv_core(), with
 * EVL-specific tweaks.
 */
int evl_net_ipv4_deliver(struct sk_buff *skb)
{
	struct iphdr *iph;
	u32 len;
	int ret;

	/*
	 * Out-of-band packets are never shared on entry and always
	 * linear. Part of this routine relies on these requirements.
	 */
	if (EVL_WARN_ON(NET, skb_shared(skb)))
		return -EINVAL;

	if (EVL_WARN_ON(NET, skb_is_nonlinear(skb)))
		return -EINVAL;

	/*
	 * Do not handle packets which were not directly targeted
	 * towards the network interface. If so, eth_type_trans() has
	 * set the packet type to PACKET_OTHERHOST.
	 */
	if (skb->pkt_type == PACKET_OTHERHOST)
		return -ENOMSG;

	/*
	 * Make sure that we have enough data in there to hold an IP
	 * header (Since out-of-band packets are always linear,
	 * skb->data_len is zero, therefore skb_headlen() is actually
	 * skb->len).
	 */
	if (skb_headlen(skb) < sizeof(*iph))
		return -EINVAL;

	iph = ip_hdr(skb);

	/*
	 * Check minimum size of an IP header in 32bit words, and IPv4
	 * signature as well.
	 */
	if (iph->ihl < 5 || iph->version != 4)
		return -EINVAL;

	if (skb_headlen(skb) < iph->ihl * sizeof(u32))
		return -EINVAL;

	/* RFC 1122: silently drop packets failing the checksum. */
	if (unlikely(ip_fast_csum(iph, iph->ihl)))
		return -EINVAL;

	/* Check for truncated packet. */
	len = ntohs(iph->tot_len);
	if (skb->len < len)
		return -EINVAL;

	if (len < iph->ihl * sizeof(u32))
		return -EINVAL;

	if (pskb_trim_rcsum(skb, len))
		return -EINVAL;

	/*
	 * The IP header should not have moved because of trimming
	 * since the skb is linear and not cloned.
	 */
	if (EVL_WARN_ON(NET, iph != ip_hdr(skb)))
		return -EINVAL;

	skb->transport_header = skb->network_header + iph->ihl * sizeof(u32);

	/*
	 * If this is an IP fragment, push it to the defragmenter for
	 * reassembly. A successful return from push_frag() passing
	 * back a valid heading skb means that the datagram is now
	 * complete.
	 */
	if (ip_is_fragment(iph)) {
		skb = push_frag(skb, skb->dev ?: skb_dst(skb)->dev);
		if (IS_ERR(skb)) {
			ret = PTR_ERR(skb);
			switch (ret) {
			case -EINPROGRESS:
			case -E2BIG:
				/*
				 * We don't want the caller to drop
				 * the skb, either because we are
				 * waiting for more data to reassemble
				 * the datagram, or we did the cleanup
				 * already.
				 */
				return 0;
			default:
				return ret;
			}
		}
	}

	/* Pass complete datagram to the next layer. */

	switch (iph->protocol) {
	case IPPROTO_UDP:
		ret = evl_net_deliver_udp(skb);
		break;
	case IPPROTO_ICMP:
		ret = evl_net_deliver_icmp(skb);
		break;
	default:
		ret = -ENOTSUPP;
	}

	/*
	 * Our caller does not know about fragmentation, and skb might
	 * have changed to point to the heading buffer after the
	 * reassembly went to completion. On error, care for releasing
	 * such buffer by ourselves.
	 */
	if (ret)
		evl_net_free_skb(skb);

	return 0;
}
