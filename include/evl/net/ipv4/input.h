/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2023 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_IPV4_INPUT_H
#define _EVL_NET_IPV4_INPUT_H

#include <linux/net.h>

struct sk_buff;

int evl_net_ipv4_deliver(struct sk_buff *skb);

void __evl_net_ipv4_gc(struct evl_net_frag_tdir *ftdir);

static inline void evl_net_ipv4_gc(struct net *net)
{
	struct evl_net_frag_tdir *ftdir = &net->oob.ipv4.ftdir;

	if (!hlist_empty(&ftdir->gc.queue))
		__evl_net_ipv4_gc(ftdir);
}

#endif /* !_EVL_NET_IPV4_INPUT_H */
