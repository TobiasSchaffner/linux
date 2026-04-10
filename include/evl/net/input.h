/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_INPUT_H
#define _EVL_NET_INPUT_H

#include <linux/list.h>
#include <evl/lock.h>
#include <uapi/evl/net/bpf-abi.h>

struct sk_buff;

struct evl_net_handler {
	void (*ingress)(struct sk_buff *skb);
};

void evl_net_do_rx(void *arg);

void evl_net_receive(struct sk_buff *skb,
		struct evl_net_handler *handler);

enum evl_net_rx_action evl_net_ether_accept(struct sk_buff *skb);

enum evl_net_rx_action evl_net_ether_accept_vlan(struct sk_buff *skb);

ssize_t evl_net_store_vlans(const char *buf, size_t len);

ssize_t evl_net_show_vlans(char *buf, size_t len);

#endif /* !_EVL_NET_INPUT_H */
