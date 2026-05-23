/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_IPV4_ICMP_H
#define _EVL_NET_IPV4_ICMP_H

#include <evl/net/socket.h>

int evl_net_deliver_icmp(struct sk_buff *skb);

int evl_net_init_icmp(struct net *net);

void evl_net_cleanup_icmp(struct net *net);

extern struct evl_net_proto evl_net_icmp_proto;

#endif /* !_EVL_NET_IPV4_ICMP_H */
