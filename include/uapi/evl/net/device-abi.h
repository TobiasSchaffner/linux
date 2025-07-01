/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Copyright (C) 2024 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_UAPI_NET_DEVICE_ABI_H
#define _EVL_UAPI_NET_DEVICE_ABI_H

#include <linux/types.h>

#define EVL_NETDEV_IOCBASE  0xef

/* Obtain the status of an oob port. */
struct evl_net_devstat {
	__u64 rx_packets;
	__u64 rx_bytes;
	__u64 tx_packets;
	__u64 tx_bytes;
	__u32 skb_size;
	__u32 skb_free;
	__u32 skb_total;
	__u32 csum_errors;
	__u32 rx_nomem;
	__u32 tx_nomem;
	union {
		__u32 oob_capable:1;	/* Driver is oob_capable */
		__u32 __flags;
	};
};

#define EVL_NDEVIOC_SETRXEBPF	_IOW(EVL_NETDEV_IOCBASE, 0, __s32 /* fd */)
#define EVL_NDEVIOC_SWITCHOFF	_IO(EVL_NETDEV_IOCBASE, 1)
#define EVL_NDEVIOC_GETSTAT	_IOW(EVL_NETDEV_IOCBASE, 2, struct evl_net_devstat)

#endif /* !_EVL_UAPI_NET_DEVICE_ABI_H */
