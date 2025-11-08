/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2021 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/inetdevice.h>
#include <net/inet_sock.h>
#include <net/inet_common.h>
#include <net/udp.h>
#include <evl/memory.h>
#include <evl/uaccess.h>
#include <evl/uio.h>
#include <evl/net/offload.h>
#include <evl/net/skb.h>
#include <evl/net/device.h>
#include <evl/net/timestamping.h>
#include <evl/net/ip.h>
#include <evl/net/ipv4.h>
#include <evl/net/ipv4/output.h>
#include <evl/net/ipv4/route.h>
#include <evl/net/ipv4/arp.h>
#include <evl/net/ipv4/udp.h>

#define EVL_NET_UDP_CACHE_SHIFT  8

/* in-band. */
static int attach_udp_socket(struct evl_socket *esk,
			struct evl_net_proto *proto, int protocol)
{
	esk->proto = proto;
	esk->protocol = protocol;
	evl_net_init_ip_socket(esk);
	INIT_LIST_HEAD(&esk->u.ip.udp.next);

	return 0;
}

/*
 * set_receive_slot - install/update a receive slot for the socket to
 * collect input. We are called whenever bind() is issued on an UDP
 * socket from the inband stack: this enables our generic cache
 * mechanism to deal with this information since it supports
 * inband-only updates, inband/oob lookups.
 *
 * @esk->sk is locked on entry.
 */
static int add_receive_slot(struct evl_socket *esk) /* inband */
{
	struct evl_cache *cache = &esk->net->oob.ipv4.udp;
	struct inet_sock *inet = inet_sk(esk->sk);
	struct evl_net_udp_rcvslot *new, *old;
	struct evl_cache_entry *entry;
	unsigned long flags;
	int ret;

	/*
	 * Allocate without holding any lock to eliminate any risk of
	 * lock order inversion. We are unlikely to find a
	 * pre-existing slot for the port (i.e. unless SO_REUSEPORT is
	 * in effect), therefore this new slot will be used in most
	 * cases.
	 */
	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	/*
	 * Binding for oob[-extended] protocols always and only
	 * happens after the inband-side binding operation was
	 * successful, so we may use the port and receive address the
	 * inband stack already parsed and checked, dealing with the
	 * hairy reuseport logic as well. Yummie.
	 */
	new->key.port = inet->inet_num;
	INIT_LIST_HEAD(&new->receivers);
	refcount_set(&new->refs, 1);
	raw_spin_lock_init(&new->lock);

	/* Lookup and insertion must be seen as atomic. */
	evl_lock_cache(cache);
	entry = evl_lookup_cache(cache, &new->key);
	if (entry) {
		/* Unlock prior to freeing the unused slot (see above). */
		evl_unlock_cache(cache);
		old = container_of(entry, struct evl_net_udp_rcvslot, entry);
		/* One user more via reuseport, account for it. */
		refcount_inc(&old->refs);
		evl_put_cache_entry(entry);
		kfree(new);
		new = old;
	} else {
		ret = evl_add_cache_entry_locked(cache, &new->entry);
		evl_unlock_cache(cache);
		if (ret) {
			kfree(new);
			return ret;
		}
	}

	raw_spin_lock_irqsave(&new->lock, flags);

	if (EVL_WARN_ON(NET, !list_empty(&esk->u.ip.udp.next)))
		list_del(&esk->u.ip.udp.next); /* Bad news ahead anyway. */

	list_add(&esk->u.ip.udp.next, &new->receivers);

	WRITE_ONCE(esk->u.ip.udp.rcv_slot, new);

	raw_spin_unlock_irqrestore(&new->lock, flags);

	return 0;
}

/*
 * drop_receive_slot - remove a receive slot previously installed by
 * add_receive_slot(). Slots a refcounted, so that SO_REUSEPORT is
 * dealt with.
 *
 * @esk->sk is either locked on entry, or not known from anyone else
 * (i.e. zombie state).
 */
static void drop_receive_slot(struct evl_socket *esk) /* inband */
{
	struct evl_cache *cache = &esk->net->oob.ipv4.udp;
	struct evl_net_udp_rcvslot *rslot;
	unsigned long flags;

	/*
	 * esk holds a reference on the receive slot referred to by
	 * udp.rcv_slot.
	 */
	rslot = READ_ONCE(esk->u.ip.udp.rcv_slot);
	if (rslot) {
		raw_spin_lock_irqsave(&rslot->lock, flags);
		list_del_init(&esk->u.ip.udp.next);
		WRITE_ONCE(esk->u.ip.udp.rcv_slot, NULL);
		raw_spin_unlock_irqrestore(&rslot->lock, flags);

		if (refcount_dec_and_test(&rslot->refs))
			evl_del_cache_entry(cache, &rslot->key);
	}
}

/*
 * destroy_udp_socket - perform cleanup on UDP socket
 * closure. This routine runs as an RCU callback.
 *
 * NOTE: sk_common_release() already ran for esk->sk, which means the
 * socket is not hashed on any inband receive port anymore. We only
 * have to release the receive slot @esk might be referring to.
 */
static void destroy_udp_socket(struct evl_socket *esk) /* inband */
{
	drop_receive_slot(esk);
}

/*
 * Bind a new UDP socket for oob usage. @esk->sk is locked by the
 * inband stack on entry. Unbinding happens when the socket is either
 * shut down or destroyed on the inband side, which is paired with our
 * shutdown() and destroy() handlers.
 */
static int bind_udp_socket(struct evl_socket *esk,
			struct sockaddr *addr,
			int len)
{
	return add_receive_slot(esk);
}

/*
 * @esk->sk is locked by the inband stack on entry.
 */
static int shutdown_udp_socket(struct evl_socket *esk, int how)
{
	/*
	 * We send RMID to any waiter although this is not actually a
	 * deletion, but what we want is the latter to unblock.
	 */
	evl_flush_wait(&esk->input_wait, EVL_T_RMID);
	evl_signal_poll_events(&esk->poll_head,	POLLIN|POLLRDNORM);
	evl_schedule();
	drop_receive_slot(esk);
	evl_net_purge_socket_input(esk);

	return 0;
}

static ssize_t offload_send_udp(struct evl_socket *esk,
				struct kvec *kvec, size_t count,
				struct sockaddr_in *in_dest)
{
	struct evl_net_offload *ofld;

	ofld = evl_alloc(sizeof(*ofld));
	if (!ofld)
		return -ENOMEM;

	ofld->kvec = *kvec;
	ofld->count = count;
	ofld->dest.in = in_dest ? *in_dest : (struct sockaddr_in){};
	ofld->destlen = in_dest ? sizeof(*in_dest) : 0;
	evl_net_offload_inband(esk, ofld, &esk->u.ip.pending_output);

	return count;
}

/*
 * Given an IPv4 address, look into our oob route and ARP front caches
 * to find an egress path. If we cannot find a route to the next hop
 * through an oob-enabled device, or we don't know the hardware
 * address of this peer, then the caller will have to pass on the
 * datagram to the in-band stack.
 */
static int find_egress_path(struct evl_socket *esk,
			__be32 daddr,
			struct evl_net_route **ertp,
			struct evl_net_arp_entry **earpp,
			struct evl_net_arp_entry *pseudo_earp,
			int msg_flags)
{
	struct evl_net_arp_entry *earp;
	struct evl_net_route *ert;
	struct sock *sk = esk->sk;
	int ret;

	ert = evl_net_route_ipv4_output(sock_net(esk->sk), daddr);
	if (!ert)
		return -ENOENT;

	/*
	 * Need a broadcast-enabled socket for using a broadcast
	 * route.
	 */
	ret = -EACCES;
	if ((ert->rt->rt_flags & RTCF_BROADCAST) &&
		!sock_flag(sk, SOCK_BROADCAST))
		goto fail;

	/*
	 *  If MSG_DONTROUTE was given, make sure the destination is
	 *  no more than one hop away from us.
	 */
	ret = -EMULTIHOP;
	if  (msg_flags & MSG_DONTROUTE && rt_nexthop(ert->rt, daddr) != daddr)
		goto fail;

	ret = -ENOENT;
	earp = evl_net_get_arp_entry_or_pseudo(ert->rt->dst.dev, daddr,
					pseudo_earp);
	if (!earp)
		goto fail;

	*ertp = ert;
	*earpp = earp;

	return 0;
fail:
	evl_net_put_route(ert);

	return ret;
}

/*
 * Calculate the UDP checksum, starting from the IP header up to the
 * full payload, including the UDP header. @skb contains the IP frame
 * heading a valid UDP datagram, which might be fragmented over
 * additional IP frames.
 */
static inline __wsum checksum_datagram(struct sk_buff *skb)
{
	__wsum csum = csum_partial(skb->data, skb->len, 0);
	struct sk_buff *fskb;

	skb_walk_frags(skb, fskb) {
		csum = csum_partial(fskb->data, fskb->len, csum);
	}

	return csum;
}

/*
 * Send a datagram - which might be fragmented - to the peer we have
 * an ARP entry for.
 */
static int send_datagram(struct sk_buff *skb, struct net_device *dev,
			struct evl_net_arp_entry *earp,
			struct evl_net_ipv4_cookie *ipc,
			__be16 dport, __be16 sport,
			size_t datalen)
{
	size_t ulen = datalen + sizeof(struct udphdr);
	struct udphdr *uh;

	/*
	 * Set up our transport header. evl_net_ipv4_build_datagram()
	 * reserved the required space in the heading skb for us.
	 */
	uh = udp_hdr(skb);
	uh->source = sport;
	uh->dest = dport;
	uh->len = htons(ulen);
	uh->check = 0; /* Caution: checksum_datagram() reads this too. */
	uh->check = csum_tcpudp_magic(ipc->saddr, ipc->daddr,
				ulen, IPPROTO_UDP, checksum_datagram(skb));
	if (uh->check == 0)
		uh->check = CSUM_MANGLED_0;

	skb->ip_summed = CHECKSUM_NONE;

	/* On error, this call releases the untransmitted buffers. */
	return evl_net_ether_transmit(dev, skb, earp->ha);
}

/* oob */
static ssize_t send_udp(struct evl_socket *esk,
			const struct user_oob_msghdr __user *u_msghdr,
			struct iovec *iov,
			size_t iovlen)
{
	struct evl_net_arp_entry *earp, pseudo_earp = { 0 };
	struct sockaddr_in in_addr, *u_in_addr;
	struct sock *sk = esk->sk;
	struct inet_sock *inet = inet_sk(sk);
	struct evl_net_ipv4_cookie ipc;
	struct evl_net_route *ert;
	struct msghdr msg = { 0 };
	struct __evl_timespec uts;
	ssize_t datalen, ret;
	enum evl_tmode tmode;
	__u32 msg_flags = 0;
	struct sk_buff *skb;
	__be32 daddr, saddr;
	__u32 namelen = 0;
	struct kvec kvec;
	ktime_t timeout;
	__u64 name_ptr;
	__be16 dport;

	if (READ_ONCE(sk->sk_shutdown) & SEND_SHUTDOWN)
		return -EPIPE;

	ret = raw_get_user(msg_flags, &u_msghdr->flags);
	if (ret)
		return -EFAULT;

	/*
	 * Unlike BSD, we accept MSG_DONTWAIT to decline waiting on
	 * skb contention, or offloading to the in-band stage.  We
	 * interpret MSG_DONTROUTE in a slightly awkward manner, in
	 * that it makes us verify that the destination is directly
	 * reachable from this host (i.e. on-link TOS). IOW, it does
	 * not affect routing, it ensures that the routing information
	 * we use is right for the task.
	 */
	if (msg_flags & ~(MSG_DONTWAIT|MSG_DONTROUTE))
		return -EINVAL;

	if (evl_socket_f_flags(esk) & O_NONBLOCK)
		msg_flags |= MSG_DONTWAIT;

	ret = raw_copy_from_user(&uts, &u_msghdr->timeout, sizeof(uts));
	if (ret)
		return -EFAULT;

	timeout = msg_flags & MSG_DONTWAIT ? EVL_NONBLOCK :
		u_timespec_to_ktime(uts);
	tmode = timeout ? EVL_ABS : EVL_REL;

	ret = raw_get_user(name_ptr, &u_msghdr->name_ptr);
	if (ret)
		return -EFAULT;

	if (name_ptr) {
		ret = raw_get_user(namelen, &u_msghdr->namelen);
		if (ret)
			return -EFAULT;
		if (namelen < sizeof(in_addr))
			return -EINVAL;
		u_in_addr = evl_valptr64(name_ptr, struct sockaddr_in);
		ret = raw_copy_from_user(&in_addr, u_in_addr, sizeof(in_addr));
		if (ret)
			return -EFAULT;
		daddr = in_addr.sin_addr.s_addr;
		dport = in_addr.sin_port;
		if (!daddr || !dport)
			return -EINVAL;
		msg.msg_name = (struct sockaddr *)&in_addr;
		msg.msg_namelen = namelen;
	} else {
		if (sk->sk_state != TCP_ESTABLISHED)
			return -EDESTADDRREQ;
		daddr = inet->inet_daddr;
		dport = inet->inet_dport;
	}

	datalen = evl_iov_flat_length(iov, iovlen);
	if (datalen == 0)
		return 0;

	/* UDP datagram cannot exceed 64k. */
	if (datalen > 65535)
		return -EMSGSIZE;

	/*
	 * Try finding an oob path for the datagram based on the
	 * routing information collected into our front caches. If
	 * none, then offload the packet to the inband stack (as a
	 * result, we may receive the missing information eventually).
	 */
	ret = find_egress_path(esk, daddr, &ert, &earp, &pseudo_earp, msg_flags);
	if (ret == -EMULTIHOP)
		return ret;	/* MSG_DONTROUTE cannot be honored. */

	if (ret) { /* No route known from the front cache - bummer. */
		/*
		 * We always charge the socket even when offloading to
		 * the in-band stack although we won't consume any
		 * oob skb for transmit in that case. This allows for
		 * contention management.
		 */
		ret = evl_charge_socket_wmem(esk, datalen, timeout, tmode);
		if (ret)
			return ret;

		ret = evl_copy_from_uio_to_kvec(iov, iovlen, datalen, &kvec);
		if (ret < 0) {
			evl_uncharge_socket_wmem(esk, datalen);
			return ret;
		}

		ret = offload_send_udp(esk, &kvec, ret, namelen ? &in_addr : NULL);
		if (ret < 0)
			return ret;
		/*
		 * EVL-specific: we had to pass on the request to the
		 * in-band stage for routing and/or MAC address
		 * resolution. So the datagram is indeed in-flight,
		 * but we cannot guarantee a bounded delay before it
		 * is written to the wire. In such a case, return
		 * -EINPROGRESS. This is a way for the caller to
		 * detect a missing peer solicitation before the
		 * latter is sent oob data.
		 */
		return -EINPROGRESS;
	}

	/*
	 * Ok, we have an oob path for that datagram. In connected
	 * mode (i.e. an address was bound to the socket), pick the
	 * bound source address. Otherwise, use the source address
	 * determined when the routing information was established
	 * then passed to evl_net_learn_ipv4_route().
	 */
	saddr = inet->inet_saddr;
	if (!saddr) {
		if (ipv4_is_multicast(daddr))
			saddr = inet->mc_addr;
		else
			saddr = ert->flowi4.saddr;
	}

	ipc.saddr = saddr;
	ipc.daddr = daddr;
	ipc.protocol = IPPROTO_UDP;
	ipc.transhdrlen = sizeof(struct udphdr);
	skb = evl_net_ipv4_build_datagram(esk, iov, iovlen, ert,
					datalen, timeout, &ipc);
	if (IS_ERR_OR_NULL(skb)) {
		ret = PTR_ERR(skb);
		goto out;
	}

	ret = send_datagram(skb, ert->rt->dst.dev, earp, &ipc,
			dport, inet->inet_sport, datalen);

	/* EIDRM is special case for receiving shutdown(2) while waiting. */
	if (ret == -EIDRM)
		ret = -EPIPE;
out:
	if (likely(earp != &pseudo_earp))
		evl_net_put_arp_entry(earp);

	evl_net_put_route(ert);

	return ret ?: datalen;
}

static ssize_t copy_datagram_to_user(struct user_oob_msghdr __user *u_msghdr,
				const struct iovec *iov,
				size_t iovlen,
				struct sk_buff *skb,
				__u32 msg_uflags)
{
	struct sockaddr_in addr, __user *u_addr;
	__u64 name_ptr, namelen;
	ssize_t ret, count;
	bool short_write;

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
			addr.sin_family = AF_INET;
			addr.sin_port = udp_hdr(skb)->source;
			addr.sin_addr.s_addr = ip_hdr(skb)->saddr;
			memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
			u_addr = evl_valptr64(name_ptr, struct sockaddr_in);
			ret = raw_copy_to_user(u_addr, &addr, sizeof(addr));
			if (ret)
				return -EFAULT;
		} else {
			if (namelen)
				return -EINVAL;
		}
	}

	skb_pull_inline(skb, sizeof(struct udphdr));

	count = evl_net_skb_to_uio(iov, iovlen, skb, sizeof(struct iphdr), &short_write);

	if (u_msghdr && short_write)
		ret = raw_put_user(msg_uflags | MSG_TRUNC, &u_msghdr->flags);

	return ret ? -EFAULT : count;
}

/* oob */
static ssize_t receive_udp(struct evl_socket *esk,
			struct user_oob_msghdr __user *u_msghdr, /* oob_read() if NULL */
			struct iovec *iov,
			size_t iovlen)
{
	__u32 msg_flags = 0, msg_uflags = 0;
	ktime_t timeout = EVL_INFINITE;
	enum evl_tmode tmode = EVL_REL;
	struct __evl_timespec uts;
	struct sk_buff *skb;
	unsigned long flags;
	ssize_t ret;

	if (READ_ONCE(esk->sk->sk_shutdown) & RCV_SHUTDOWN)
		return 0;

	if (evl_socket_f_flags(esk) & O_NONBLOCK)
		msg_flags |= MSG_DONTWAIT;

	if (u_msghdr) {
		ret = raw_get_user(msg_uflags, &u_msghdr->flags);
		if (ret)
			return -EFAULT;

		msg_flags |= msg_uflags;

		if (msg_flags & ~(MSG_DONTWAIT|MSG_TIMESTAMP))
			return -EINVAL;

		ret = raw_copy_from_user(&uts, &u_msghdr->timeout,
					sizeof(uts));
		if (ret)
			return -EFAULT;

		if (msg_flags & MSG_DONTWAIT) {
			timeout = EVL_NONBLOCK;
		} else {
			timeout = u_timespec_to_ktime(uts);
			if (timeout)
				tmode = EVL_ABS;
		}

		if (msg_flags & MSG_TIMESTAMP)
			return evl_collect_socket_iots(esk, u_msghdr, msg_uflags,
						iov, iovlen, timeout, tmode);
	}

	do {
		raw_spin_lock_irqsave(&esk->input_wait.wchan.lock, flags);

		if (list_empty(&esk->input))
			goto wait;

		skb = list_get_entry(&esk->input, struct sk_buff, list);

		raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);

		/* Record the timestamps if required. */
		ret = 0;
		if (READ_ONCE(esk->timestamping) & EVL_SOF_TIMESTAMP_RX)
			ret = evl_copy_iots_rx(skb, u_msghdr);

		if (likely(!ret))
			ret = copy_datagram_to_user(u_msghdr, iov, iovlen, skb, msg_uflags);

		evl_net_uncharge_skb_rmem(skb);
		evl_net_free_skb(skb);
		return ret;

	wait:
		if (msg_flags & MSG_DONTWAIT) {
			raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
			return -EWOULDBLOCK;
		}

		evl_add_wait_queue(&esk->input_wait, timeout, tmode);
		raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
		ret = evl_wait_schedule(&esk->input_wait);
	} while (!ret);

	/* EIDRM is special case for receiving shutdown(2) while waiting. */
	return ret == -EIDRM ? 0 : ret;
}

/* oob */
static __poll_t poll_udp(struct evl_socket *esk,
			struct oob_poll_wait *wait)
{
	__poll_t ret = POLLIN|POLLRDNORM|POLLOUT|POLLWRNORM;
	unsigned long flags;
	u8 shutdown;

	/* Enqueue, then test. */
	evl_poll_watch(&esk->poll_head, wait, NULL);

	shutdown = READ_ONCE(esk->sk->sk_shutdown);
	if (shutdown & RCV_SHUTDOWN)
		ret |= EPOLLRDHUP;
	if (shutdown == SHUTDOWN_MASK)
		ret = EPOLLHUP;
	else if (shutdown & SEND_SHUTDOWN)
		ret &= ~POLLOUT|POLLWRNORM;

	rcu_read_lock();	/* For checking timestamps. */

	/*
	 * We might have lingering timestamps to consume, check this
	 * unconditionally, regardless of whether messages are
	 * pending.
	 */
	if (!__evl_test_socket_iots(esk)) {
		raw_spin_lock_irqsave(&esk->input_wait.wchan.lock, flags);
		if (list_empty(&esk->input))
			ret &= ~POLLIN|POLLRDNORM;
		raw_spin_unlock_irqrestore(&esk->input_wait.wchan.lock, flags);
	}

	rcu_read_unlock();

	return ret;
}

/* in-band */
static void handle_udp_inband(struct evl_socket *esk)
{
	struct evl_net_offload *ofld, *n;
	struct sock *sk = esk->sk;
	unsigned long flags;
	LIST_HEAD(tmp);
	int ret;

	/* Process pending output. */

	raw_spin_lock_irqsave(&esk->oob_lock, flags);
	list_splice_init(&esk->u.ip.pending_output, &tmp);
	raw_spin_unlock_irqrestore(&esk->oob_lock, flags);

	list_for_each_entry_safe(ofld, n, &tmp, next) {
		struct msghdr msg = { 0 };
		list_del(&ofld->next);
		msg.msg_namelen = ofld->destlen;
		if (msg.msg_namelen)
			msg.msg_name = (struct sockaddr *)&ofld->dest.in;
		ret = kernel_sendmsg(sk->sk_socket, &msg,
				&ofld->kvec, 1, ofld->count);
		evl_free(ofld->kvec.iov_base);
		evl_uncharge_socket_wmem(esk, ofld->count);
		evl_free(ofld);
	}
}

static inline bool __validate_checksum(struct sk_buff *skb, u16 ulen, __sum16 check)
{
	__sum16 sum;

	/*
	 * If the hw computed the checksum, combine and check with the
	 * pseudo-header.
	 */
	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		if (!csum_tcpudp_magic(ip_hdr(skb)->saddr, ip_hdr(skb)->daddr,
					ulen, IPPROTO_UDP, skb->csum)) {
			skb->csum_valid = 1;
			return true;
		}
		/* If the hw produced a bad checksum, check by ourselves. */
	}

	sum = csum_tcpudp_magic(ip_hdr(skb)->saddr, ip_hdr(skb)->daddr,
				ulen, IPPROTO_UDP, checksum_datagram(skb));
	if (!sum)
		sum = CSUM_MANGLED_0;
	/*
	 * Tricky: checksumming here although CHECKSUM_COMPLETE was
	 * set means that we've just found out that the hardware
	 * checksum was invalid. If our software-computed checksum is
	 * valid instead, then we disagree with the hardware. This
	 * means either the original hardware checksum is incorrect or
	 * we screwed up skb->csum when moving skb->data around, which
	 * is quite bad news either way.
	 */
	sum -= check;
	if (!sum && skb->ip_summed == CHECKSUM_COMPLETE)
		netdev_rx_csum_fault(skb->dev, skb);

	skb->csum = sum;
	skb->ip_summed = CHECKSUM_COMPLETE;
	skb->csum_complete_sw = 1;
	skb->csum_valid = !sum;

	return skb->csum_valid;
}

static inline bool validate_checksum(struct sk_buff *skb, u16 ulen, __sum16 check)
{
	skb->csum_valid = 0;

	if (__skb_checksum_validate_needed(skb, true, check))
		return __validate_checksum(skb, ulen, check);

	return true;
}

static inline bool verify_checksum(struct sk_buff *skb)
{
	struct udphdr *uh = udp_hdr(skb);
	unsigned short ulen;
	__sum16 check;

	ulen = ntohs(uh->len);

	if (ulen < sizeof(*uh))
		return false;	/* Short packet, drop it. */

	check = uh->check;
	uh->check = 0;

	return validate_checksum(skb, ulen, check);
}

/*
 * deliver_datagram - push an incoming datagram to the input queue of
 * interested receiver(s). @skb is not part of any queue, however it
 * might have a frag list. We may reuse skb->list only for the heading
 * @skb, but not for its frags, this is ok.
 */
static int deliver_datagram(struct sk_buff *skb)
{
	struct net *net = dev_net(skb->dev);
	struct evl_cache *cache = &net->oob.ipv4.udp;
	const struct iphdr *iph = ip_hdr(skb);
	struct udphdr *uh = udp_hdr(skb);
	struct evl_net_udp_rcvslot *rslot;
	struct evl_cache_entry *entry;
	struct __evl_net_udp_key key;
	struct evl_socket *esk;
	unsigned long flags;
	bool mbcast;
	int ret = 0;

	if (skb_is_oob_timestamped(skb))
		skb_shinfo_oob(skb)->delivery_time = evl_ktime_monotonic();

	rcu_read_lock();

	key.port = ntohs(uh->dest);
	entry = evl_lookup_cache(cache, &key);
	if (!entry)
		goto out;

	rslot = container_of(entry, struct evl_net_udp_rcvslot, entry);
	/*
	 * We don't do input routing, the final destination is
	 * directly known from the IP header.
	 */
	mbcast = ipv4_is_multicast(iph->daddr) || ipv4_is_lbcast(iph->daddr);

	/*
	 * Locking order: rslot->lock first, wchan->lock next.  You
	 * have been warned.
	 */
	raw_spin_lock_irqsave(&rslot->lock, flags);

	list_for_each_entry(esk, &rslot->receivers, u.ip.udp.next) {
		struct inet_sock *inet = inet_sk(esk->sk);
		struct sk_buff *qskb;
		int bound_if;

		if ((inet->inet_daddr != iph->saddr && inet->inet_daddr) ||
			(inet->inet_dport != uh->source && inet->inet_dport) ||
			(inet->inet_rcv_saddr && inet->inet_rcv_saddr != iph->daddr))
			continue;

		bound_if = READ_ONCE(esk->sk->sk_bound_dev_if);
		if (bound_if && skb->dev->ifindex != bound_if)
			continue;

		qskb = skb;
		if (mbcast && !list_is_last(&esk->u.ip.udp.next, &rslot->receivers)) {
			qskb = skb_oob_clone(skb);
			if (qskb == NULL) {
				evl_flush_wait(&esk->input_wait, EVL_T_NOMEM);
				continue;
			}
		}

		/*
		 * This datagram may be delivered unless that socket
		 * may not consume more memory, in which case we skip
		 * delivery and try with the next receiver. On error,
		 * we should not free the received skb, only its
		 * clones.
		 */
		if (!evl_net_charge_skb_rmem(esk, qskb)) {
			if (qskb != skb)
				evl_net_free_skb(qskb);
			continue;
		}

		raw_spin_lock(&esk->input_wait.wchan.lock);

		list_add_tail(&qskb->list, &esk->input);
		if (evl_wait_active(&esk->input_wait))
			evl_wake_up_head(&esk->input_wait);

		raw_spin_unlock(&esk->input_wait.wchan.lock);

		evl_signal_poll_events(&esk->poll_head,	POLLIN|POLLRDNORM);
		ret++;

		if (!mbcast)
			break;
	}

	raw_spin_unlock_irqrestore(&rslot->lock, flags);
out:
	rcu_read_unlock();

	evl_schedule();

	return ret;
}

int evl_net_deliver_udp(struct sk_buff *skb)
{
	if (unlikely(sizeof(struct udphdr) > skb->len))
		return -EINVAL;	/* Obviously garbled, drop that. */

	if (!verify_checksum(skb)) {
		struct evl_netdev_state *est = evl_net_get_state(skb->dev);
		evl_counter_inc_careful(&est->stats.csum_errors);
		return -EINVAL;
	}

	return deliver_datagram(skb) ? 0 : -ESRCH;
}

static u32 hash_udp_slot(const void *key)
{
	const struct __evl_net_udp_key *k = key;

	return jhash2((const u32 *)k, sizeof(*k) / sizeof(u32), 0);
}

static bool eq_udp_slot(const struct evl_cache_entry *entry,
			const void *key)
{
	const struct __evl_net_udp_key *k = key;
	const struct evl_net_udp_rcvslot *rslot =
		container_of(entry, struct evl_net_udp_rcvslot, entry);

	return rslot->key.port == k->port;
}

static char *format_udp_key(const struct evl_cache_entry *entry)
{
	const struct evl_net_udp_rcvslot *rslot =
		container_of(entry, struct evl_net_udp_rcvslot, entry);

	return kasprintf(GFP_ATOMIC, "%u", rslot->key.port);
}

static const void *get_udp_key(const struct evl_cache_entry *entry)
{
	const struct evl_net_udp_rcvslot *rslot =
		container_of(entry, struct evl_net_udp_rcvslot, entry);

	return &rslot->key;
}

static void drop_udp_slot(struct evl_cache_entry *entry) /* in-band */
{
	const struct evl_net_udp_rcvslot *rslot =
		container_of(entry, struct evl_net_udp_rcvslot, entry);

	kfree(rslot);
}

static struct evl_cache_ops udp_cache_ops = {
	.hash		= hash_udp_slot,
	.eq		= eq_udp_slot,
	.get_key	= get_udp_key,
	.format_key	= format_udp_key,
	.drop		= drop_udp_slot,
};

int evl_net_init_udp(struct net *net)
{
	struct oob_net_state *nets = &net->oob;
	struct evl_cache *cache;

	/* Cache of active UDP4 receive slots. */
	cache = &nets->ipv4.udp;
	cache->ops = &udp_cache_ops;
	cache->init_shift = EVL_NET_UDP_CACHE_SHIFT;
	cache->name = "udp_rcv_slots";

	return evl_init_cache(cache);
}

void evl_net_cleanup_udp(struct net *net)
{
	struct oob_net_state *nets = &net->oob;

	evl_flush_cache(&nets->ipv4.udp);
}

struct evl_net_proto evl_net_udp_proto = {
	.attach	= attach_udp_socket,
	.destroy = destroy_udp_socket,
	.bind = bind_udp_socket,
	.shutdown = shutdown_udp_socket,
	.solicit = evl_net_ipv4_solicit,
	.oob_send = send_udp,
	.oob_poll = poll_udp,
	.oob_receive = receive_udp,
	.handle_offload = handle_udp_inband,
};
