/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_TAP_H
#define _EVL_NET_TAP_H

struct net_device;
struct sk_buff;

void evl_net_tap_in(struct net_device *dev, struct sk_buff *skb);

void evl_net_tap_out(struct net_device *dev, struct sk_buff *skb);

void evl_net_init_taps(void);

#endif /* !_EVL_NET_TAP_H */
