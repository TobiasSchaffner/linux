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

/* Get fildes on a network device. */
struct evl_net_devopen {
	__u64 name_ptr;		/* (const char __user *name) */
	__s32 fd;		/* [out] */
};

#define EVL_NETIOC_DEVOPEN	_IOWR(EVL_NETDEV_IOCBASE, 0, struct evl_net_devopen)

#endif /* !_EVL_UAPI_NET_NET_ABI_H */
