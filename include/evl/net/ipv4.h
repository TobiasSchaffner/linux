/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2023 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_IPV4_H
#define _EVL_NET_IPV4_H

#include <linux/list.h>
#include <linux/inet.h>
#include <evl/net/socket.h>
#include <evl/net/ip.h>

struct sk_buff;
struct sockaddr;
struct timespec64;
struct notifier_block;

struct evl_net_ipv4_cookie {
	__be32 saddr;		/* Source IP */
	__be32 daddr;		/* Destination IP */
	__u8 protocol;		/* Internet protocol identifier  */
	int transhdrlen;	/* Transport header length */
};

int evl_net_ipv4_handle_event(struct notifier_block *nb,
			unsigned long event, void *arg);

int evl_net_ipv4_deliver(struct sk_buff *skb);

int evl_net_init_ipv4(struct net *net);

void evl_net_cleanup_ipv4(struct net *net);

int evl_net_ipv4_add_device(struct net_device *dev);

void evl_net_ipv4_remove_device(struct net_device *dev);

int evl_net_ipv4_solicit(struct net *net,
			struct net_device *dev,
			struct sockaddr_unsized *addr, int flags);

__be32 evl_net_ipv4_devaddr(const struct net_device *dev);

extern int evl_net_ipv4_solicit_timeout;

extern struct evl_socket_domain evl_net_ipv4;

#endif /* !_EVL_NET_IPV4_H */
