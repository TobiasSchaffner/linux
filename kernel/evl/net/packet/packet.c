/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/poll.h>
#include <linux/if_vlan.h>
#include <linux/err.h>
#include <linux/ip.h>
#include <net/sock.h>
#include <evl/lock.h>
#include <evl/thread.h>
#include <evl/wait.h>
#include <evl/poll.h>
#include <evl/sched.h>
#include <evl/uio.h>
#include <evl/net/socket.h>
#include <evl/net/packet.h>
#include <evl/net/input.h>
#include <evl/net/output.h>
#include <evl/net/device.h>
#include <evl/net/skb.h>
#include <evl/net/timestamping.h>
#include <evl/uaccess.h>

void evl_net_init_packet(struct net *net)
{
	struct oob_net_state *nets = &net->oob;

	evl_init_rculist(&nets->packet.ipv4_listeners);
	evl_init_rculist(&nets->packet.all_listeners);
}

void evl_net_cleanup_packet(struct net *net)
{
	struct oob_net_state *nets = &net->oob;

	EVL_WARN_ON(NET, !evl_rculist_empty(&nets->packet.ipv4_listeners));
	EVL_WARN_ON(NET, !evl_rculist_empty(&nets->packet.all_listeners));
}

/*
 * __packet_deliver - Deliver a packet to every listener.
 *
 * If @protocol is ETH_P_ALL, every socket bound to the catch-all
 * protocol receives a clone of @skb, leaving the latter
 * unconsumed. Otherwise, a single socket which listens to the
 * specific @protocol receives @skb which is consumed by the
 * operation.
 */
static bool __packet_deliver(struct evl_rculist *rxq, /* oob, hard irqs ON */
			struct sk_buff *skb, int protocol)
{
	struct net_device *dev = skb->dev;
	bool delivered = false;
	struct evl_socket *esk;
	struct sk_buff *qskb;
	unsigned long flags;
	bool force_unbound;
	int bound_if;

	rcu_read_lock();

	evl_rculist_for_each_entry(esk, rxq, u.packet.next) {
		force_unbound = READ_ONCE(esk->u.packet.force_unbound);
		bound_if = READ_ONCE(esk->u.packet.bound_if);
		if (force_unbound || (bound_if && bound_if != dev->ifindex))
			continue;

		qskb = skb;
		if (protocol == ETH_P_ALL) {
			qskb = skb_oob_clone(skb);
			if (qskb == NULL) {
				evl_flush_wait(&esk->input_wait, EVL_T_NOMEM);
				continue;
			}
		}

		/*
		 * This packet may be delivered unless that socket may
		 * not consume more memory, in which case we skip
		 * delivery and try with the next receiver. On error,
		 * we should not free the received skb, only its
		 * clones.
		 */
		if (!evl_net_charge_skb_rmem(esk, qskb)) {
			if (qskb != skb)
				evl_net_free_skb(qskb);
			continue;
		}

		raw_spin_lock_irqsave(&esk->input_wait.wchan.lock, flags);

		list_add_tail(&qskb->list, &esk->input);
		if (evl_wait_active(&esk->input_wait))
			evl_wake_up_head(&esk->input_wait);

		raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);

		evl_signal_poll_events(&esk->poll_head,	POLLIN|POLLRDNORM);
		delivered = true;
		if (protocol != ETH_P_ALL)
			break;
	}

	rcu_read_unlock();

	return delivered;
}

static struct evl_rculist *get_rxq(struct net *net, int protocol)
{
	struct oob_net_state *nets = &net->oob;

	switch (protocol) {
	case ETH_P_IP:
		return &nets->packet.ipv4_listeners;
	case ETH_P_ALL:
		return &nets->packet.all_listeners;
	default:
		return NULL;
	}
}

static bool packet_deliver(struct sk_buff *skb, int protocol) /* oob */
{
	struct evl_rculist *rxq = get_rxq(dev_net(skb->dev), protocol);

	if (!rxq)
		return false;

	return __packet_deliver(rxq, skb, protocol);
}

/**
 *	evl_net_packet_deliver - deliver a packet to the raw interface
 *	tap
 *
 *	Deliver a copy of @skb to every socket accepting all protocols
 *	(ETH_P_ALL) if any, and/or @skb to the heading socket waiting
 *	for skb->protocol.
 *
 *	@skb the packet to deliver, not linked to any upstream
 *	queue.
 *
 *      Returns true if @skb was queued.
 *
 *	Caller must call evl_schedule().
 */
bool evl_net_packet_deliver(struct sk_buff *skb) /* oob */
{
	bool all_delivered;

	if (skb_is_oob_timestamped(skb))
		skb_shinfo_oob(skb)->delivery_time = evl_ktime_monotonic();

	all_delivered = packet_deliver(skb, ETH_P_ALL);

	if (packet_deliver(skb, ntohs(skb->protocol)))
		return true;

	/*
	 * If we are going to short-circuit the receive sequence, we
	 * have to free the buffer since the caller won't do it.
	 */
	if (all_delivered)
		evl_net_free_skb(skb);

	return all_delivered;
}

static void do_bind(struct evl_socket *esk, int protocol)
{
	struct evl_rculist *rxq = get_rxq(esk->net, protocol);

	esk->protocol = protocol;

	if (rxq)
		evl_add_rculist_entry(rxq, &esk->u.packet.next);
}

/* in-band. */
static int attach_packet_socket(struct evl_socket *esk,
				struct evl_net_proto *proto, int protocol)
{
	/* If protocol is zero, we won't capture any packet yet. */
	esk->proto = proto;
	do_bind(esk, protocol);

	return 0;
}

static void do_unbind(struct evl_socket *esk)
{
	struct evl_rculist *rxq = get_rxq(esk->net, esk->protocol);

	if (rxq) {
		evl_del_rculist_entry(rxq, &esk->u.packet.next);
		esk->protocol = 0;
	}
}

/* in-band, esk->lock held or __sk_destruct() */
static void destroy_packet_socket(struct evl_socket *esk)
{
	evl_net_dev_unbind(esk, esk->u.packet.bound_if);
	do_unbind(esk);
}

/* in-band */
static int bind_packet_socket(struct evl_socket *esk,
			struct sockaddr_unsized *addr,
			int len)
{
	struct net_device *bound_dev;
	struct sockaddr_ll *sll;
	int new_if, old_if;

	if (len != sizeof(*sll))
		return -EINVAL;

	sll = (struct sockaddr_ll *)addr;
	if (sll->sll_family != AF_PACKET)
		return -EINVAL;

	if (!get_rxq(esk->net, ntohs(sll->sll_protocol)))
		return -EINVAL;

	mutex_lock(&esk->lock);

	old_if = esk->u.packet.bound_if;

	new_if = sll->sll_ifindex;
	if (new_if) {
		bound_dev = evl_net_get_dev_by_index(esk->net, new_if);
		if (!bound_dev) {
			mutex_unlock(&esk->lock);
			return -EINVAL;
		}
		evl_net_dev_unbind(esk, old_if);
		evl_net_dev_bind(bound_dev, esk);
		evl_net_put_dev(bound_dev);
		WRITE_ONCE(esk->u.packet.force_unbound, false);
	} else {
		evl_net_dev_unbind(esk, old_if);
	}

	WRITE_ONCE(esk->u.packet.bound_if, new_if);

	/* Rebind if we track a different protocol. */
	if (esk->protocol != ntohs(sll->sll_protocol)) {
		do_unbind(esk);
		do_bind(esk, ntohs(sll->sll_protocol));
	}

	mutex_unlock(&esk->lock);

	return 0;
}

static void force_unbind_packet_socket(struct evl_socket *esk)
{
	WRITE_ONCE(esk->u.packet.force_unbound, true);
	WRITE_ONCE(esk->u.packet.bound_if, 0);
}

static struct net_device *get_netif(struct evl_socket *esk)
{
	return evl_net_get_dev_by_index(esk->net,
					esk->u.packet.bound_if);
}

static struct net_device *find_xmit_device(struct evl_socket *esk,
			const struct user_oob_msghdr __user *u_msghdr)
{
	struct sockaddr_ll __user *u_addr;
	__u64 name_ptr = 0, namelen = 0;
	struct sockaddr_ll addr;
	struct net_device *dev;
	int ret;

	if (u_msghdr) {
		ret = raw_get_user(name_ptr, &u_msghdr->name_ptr);
		if (ret)
			return ERR_PTR(-EFAULT);

		ret = raw_get_user(namelen, &u_msghdr->namelen);
		if (ret)
			return ERR_PTR(-EFAULT);

		if (!name_ptr) {
			if (namelen)
				return ERR_PTR(-EINVAL);

			dev = get_netif(esk);
		} else {
			if (namelen < sizeof(addr))
				return ERR_PTR(-EINVAL);

			u_addr = evl_valptr64(name_ptr, struct sockaddr_ll);
			ret = raw_copy_from_user(&addr, u_addr, sizeof(addr));
			if (ret)
				return ERR_PTR(-EFAULT);

			if (addr.sll_family != AF_PACKET &&
				addr.sll_family != AF_UNSPEC)
				return ERR_PTR(-EINVAL);

			dev = evl_net_get_dev_by_index(esk->net, addr.sll_ifindex);
		}
	} else {
		dev = get_netif(esk);
	}

	if (dev == NULL)
		return ERR_PTR(-ENXIO);

	return dev;
}

/* oob */
static ssize_t send_packet(struct evl_socket *esk,
			const struct user_oob_msghdr __user *u_msghdr, /* oob_write() if NULL */
			struct iovec *iov,
			size_t iovlen)
{
	struct __evl_timespec __user *u_timeout;
	struct net_device *dev, *real_dev;
	ktime_t timeout = EVL_INFINITE;
	enum evl_tmode tmode = EVL_REL;
	struct sk_buff *skb;
	__u32 msg_flags = 0;
	ssize_t ret, count;
	__u64 timeout_ptr;
	size_t rem;

	if (u_msghdr) {
		ret = raw_get_user(msg_flags, &u_msghdr->flags);
		if (ret)
			return -EFAULT;

		if (msg_flags & ~MSG_DONTWAIT)
			return -EINVAL;

		if (evl_socket_f_flags(esk) & O_NONBLOCK)
			msg_flags |= MSG_DONTWAIT;

		if (msg_flags & MSG_DONTWAIT) {
			timeout = EVL_NONBLOCK;
		} else {
			/* Fetch the timeout on obtaining a buffer from the TX pool. */
			ret = raw_copy_from_user(&timeout_ptr,
					&u_msghdr->timeout_ptr, sizeof(timeout_ptr));
			if (ret)
				return -EFAULT;

			u_timeout = evl_valptr64(timeout_ptr, struct __evl_timespec);
			if (u_timeout) {
				ret = evl_fetch_utimespec(u_timeout, &timeout, &tmode);
				if (ret)
					return ret;
			}
		}
	}

	/* Determine the xmit interface. */
	dev = find_xmit_device(esk, u_msghdr);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	/*
	 * If @dev is a VLAN device, then @real_dev cannot be stale
	 * until @dev goes down per the in-band refcounting
	 * guarantee. Since we hold a crossing reference on @real_dev,
	 * it cannot go down as it would need to pass the crossing
	 * first, so @real_dev cannot go stale until we are done.
	 */
	real_dev = evl_net_real_dev(dev);

	skb = evl_net_alloc_skb(dev, timeout, tmode);
	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		goto out;
	}

	if (READ_ONCE(esk->timestamping) & EVL_SOF_TIMESTAMP_TX) {
		skb_shinfo_oob(skb)->delivery_time = evl_ktime_monotonic();
		skb_mark_oob_timestamped(skb);
	}

	skb_reset_mac_header(skb);
	skb->priority = READ_ONCE(esk->sk->sk_priority);

	count = evl_copy_from_uio(iov, iovlen, skb->data, skb_tailroom(skb), &rem);
	if (rem || count + dev->hard_header_len + VLAN_HLEN > READ_ONCE(dev->mtu))
		ret = -EMSGSIZE;
	else if (!dev_validate_header(dev, skb->data, count))
		ret = -EINVAL;

	if (ret < 0)
		goto cleanup;

	skb_put(skb, count);
	skb->protocol = dev_parse_header_protocol(skb);
	skb_set_network_header(skb, real_dev->hard_header_len);

	/*
	 * Charge the socket with the memory consumption of skb,
	 * waiting for the output to drain if needed. The latter might
	 * fail if we got forcibly unblocked while waiting for the
	 * output contention to end, or the caller asked for a
	 * non-blocking operation while such contention was ongoing.
	 */
	ret = evl_net_charge_skb_wmem(esk, skb, timeout, tmode);
	if (ret)
		goto cleanup;

	ret = evl_net_dev_transmit_raw(dev, skb);
	if (ret) {
		evl_net_uncharge_skb_wmem(skb);
		goto cleanup;
	}

	ret = count;
out:
	evl_net_put_dev(dev);

	return ret;
cleanup:
	evl_net_free_skb(skb);
	goto out;
}

static ssize_t copy_packet_to_user(struct user_oob_msghdr __user *u_msghdr, /* oob_read() if NULL */
				const struct iovec *iov,
				size_t iovlen,
				struct sk_buff *skb,
				__u32 msg_uflags)
{
	struct sockaddr_ll addr, __user *u_addr;
	__u64 name_ptr, namelen;
	ssize_t ret, count;

	if (u_msghdr) {
		ret = raw_get_user(name_ptr, &u_msghdr->name_ptr);
		if (ret)
			return -EFAULT;

		ret = raw_get_user(namelen, &u_msghdr->namelen);
		if (ret)
			return -EFAULT;

		if (name_ptr) {
			if (namelen != sizeof(addr)) {
				if (namelen < sizeof(addr))
					return -EINVAL;
				ret = raw_put_user(sizeof(addr), &u_msghdr->namelen);
				if (ret)
					return -EFAULT;
			}
			addr.sll_family = AF_PACKET;
			addr.sll_protocol = skb->protocol;
			addr.sll_ifindex = skb->dev->ifindex;
			addr.sll_hatype = skb->dev->type;
			addr.sll_pkttype = skb->pkt_type;
			addr.sll_halen = dev_parse_header(skb, addr.sll_addr);
			u_addr = evl_valptr64(name_ptr, struct sockaddr_ll);
			ret = raw_copy_to_user(u_addr, &addr, sizeof(addr));
			if (ret)
				return -EFAULT;
		} else {
			if (namelen)
				return -EINVAL;
		}
	}

	count = evl_copy_to_uio(iov, iovlen, skb->data, skb->len);

	if (u_msghdr && count < skb->len) {
		ret = raw_put_user(msg_uflags | MSG_TRUNC, &u_msghdr->flags);
		if (ret)
			return -EFAULT;
	}

	return count;
}

/* oob */
static ssize_t receive_packet(struct evl_socket *esk,
			struct user_oob_msghdr __user *u_msghdr, /* oob_read() if NULL */
			struct iovec *iov,
			size_t iovlen)
{
	struct __evl_timespec __user *u_timeout;
	__u32 msg_flags = 0, msg_uflags = 0;
	ktime_t timeout = EVL_INFINITE;
	enum evl_tmode tmode = EVL_REL;
	struct sk_buff *skb;
	unsigned long flags;
	__u64 timeout_ptr;
	ssize_t ret;

	if (evl_socket_f_flags(esk) & O_NONBLOCK)
		msg_flags |= MSG_DONTWAIT;

	if (u_msghdr) {
		ret = raw_get_user(msg_uflags, &u_msghdr->flags);
		if (ret)
			return -EFAULT;

		msg_flags |= msg_uflags;

		/* No MSG_TRUNC on recv, too much of a kludge. */
		if (msg_flags & ~(MSG_DONTWAIT|MSG_TIMESTAMP))
			return -EINVAL;

		if (msg_flags & MSG_DONTWAIT) {
			timeout = EVL_NONBLOCK;
		} else {
			ret = raw_copy_from_user(&timeout_ptr,
					&u_msghdr->timeout_ptr, sizeof(timeout_ptr));
			if (ret)
				return -EFAULT;

			u_timeout = evl_valptr64(timeout_ptr, struct __evl_timespec);
			if (u_timeout) {
				ret = evl_fetch_utimespec(u_timeout, &timeout, &tmode);
				if (ret)
					return ret;
			}
		}

		if (msg_flags & MSG_TIMESTAMP)
			return evl_collect_socket_iots(esk, u_msghdr, msg_uflags,
						iov, iovlen, timeout, tmode);
	}

	do {
		raw_spin_lock_irqsave(&esk->input_wait.wchan.lock, flags);

		if (!list_empty(&esk->input)) {
			skb = list_get_entry(&esk->input, struct sk_buff, list);
			raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
			/* Record the timestamps if required. */
			ret = 0;
			if (READ_ONCE(esk->timestamping) & EVL_SOF_TIMESTAMP_RX)
				ret = evl_copy_iots_rx(skb, u_msghdr);
			if (likely(!ret)) {
				/* Restore the MAC header. */
				skb_push(skb, skb->data - skb_mac_header(skb));
				ret = copy_packet_to_user(u_msghdr, iov, iovlen, skb, msg_uflags);
			}
			evl_net_uncharge_skb_rmem(skb);
			evl_net_free_skb(skb);
			return ret;
		}

		if (msg_flags & MSG_DONTWAIT) {
			raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
			return -EWOULDBLOCK;
		}

		evl_add_wait_queue(&esk->input_wait, timeout, tmode);
		raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
		ret = evl_wait_schedule(&esk->input_wait);
	} while (!ret);

	return ret;
}

/* oob */
static __poll_t poll_packet(struct evl_socket *esk,
			struct oob_poll_wait *wait)
{
	struct evl_netdev_state *est;
	struct net_device *dev;
	__poll_t ret = 0;

	/* Enqueue, then test. */
	evl_poll_watch(&esk->poll_head, wait, NULL);
	/* Check whether we have datagrams and/or timestamps to read. */
	if (!list_empty(&esk->input) || evl_test_socket_iots(esk))
		ret = POLLIN|POLLRDNORM;

	dev = get_netif(esk);
	if (dev) {
		est = evl_net_get_state(dev);
		evl_poll_watch(&est->poll_head, wait, NULL);
		/* FIXME: Assume we can always TX, which is too optimistic. */
		ret |= POLLOUT|POLLWRNORM;
		evl_net_put_dev(dev);
	}

	return ret;
}

static struct evl_net_proto packet_proto = {
	.attach		= attach_packet_socket,
	.destroy	= destroy_packet_socket,
	.bind		= bind_packet_socket,
	.force_unbind	= force_unbind_packet_socket,
	.connect	= evl_socket_no_connect,
	.shutdown	= evl_socket_no_shutdown,
	.ioctl		= evl_socket_no_ioctl,
	.solicit	= evl_socket_no_solicit,
	.oob_send	= send_packet,
	.oob_receive	= receive_packet,
	.oob_poll	= poll_packet,
	.handle_offload	= evl_socket_no_offload,
};

static struct evl_net_proto *
find_packet_proto(int protocol,	struct evl_net_proto *default_proto)
{
	switch (protocol) {
	case ETH_P_ALL:
	case ETH_P_IP:
		return &packet_proto;
	case 0:
		return default_proto;
	default:
		return NULL;
	}
}

static struct evl_net_proto *match_packet_domain(int type, int protocol)
{
	static struct evl_net_proto *proto;

	proto = find_packet_proto(protocol, &packet_proto);
	if (proto == NULL)
		return NULL;

	if (type != SOCK_RAW)
		return ERR_PTR(-ESOCKTNOSUPPORT);

	return proto;
}

struct evl_socket_domain evl_net_packet = {
	.af_domain = AF_PACKET,
	.match = match_packet_domain,
};
