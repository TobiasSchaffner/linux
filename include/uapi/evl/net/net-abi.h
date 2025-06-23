/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Copyright (C) 2024 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_UAPI_NET_NET_ABI_H
#define _EVL_UAPI_NET_NET_ABI_H

#include <evl/types-abi.h>

#define EVL_NET_DEV		"net"

#define EVL_NET_IOCBASE  0xf0

/* Turn on the oob port of a given device. */
struct evl_net_devparams {
	__u64 name_ptr;		/* (const char __user *name) */
	__u64 poolsz;
	__u64 bufsz;
	__s32 fd;		/* [out] */
};

/* Get fildes on an open oob port. */
struct evl_net_devfd {
	__u64 name_ptr;		/* (const char __user *name) */
	__s32 fd;		/* [out] */
};

#define EVL_NET_GETDEVFD	_IOWR(EVL_NETDEV_IOCBASE, 0, struct evl_net_devfd)
#define EVL_NET_OPENPORT	_IOWR(EVL_NETDEV_IOCBASE, 1, struct evl_net_devparams)

#endif /* !_EVL_UAPI_NET_NET_ABI_H */
