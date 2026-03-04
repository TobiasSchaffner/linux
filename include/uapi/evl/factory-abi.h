/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Copyright (C) 2018 Philippe Gerum <rpm@xenomai.org>
 */

#ifndef _EVL_UAPI_FACTORY_ABI_H
#define _EVL_UAPI_FACTORY_ABI_H

#include <linux/types.h>

#define EVL_FACTORY_IOCBASE	'f'

struct evl_element_ids {
	__u32 minor;
	__u32 fundle;		/* fundle_t */
	__u32 sstate_offset;
};

/* The core only uses bits 16-31, rest is available to libevl. */

/*
 * Element is published to /dev/evl for any authorized process to open
 * file descriptors on it. Implies EVL_CLONE_SHAREABLE.
 */
#define EVL_CLONE_PUBLIC	(1 << 16)
/*
 * Element is not published to /dev/evl. Nevertheless, it does have an
 * entry in sysfs like public elements and may be shared between
 * processes if EVL_CLONE_SHAREABLE is set.
 */
#define EVL_CLONE_PRIVATE	(0 << 16)
/*
 * Element has built-in observability, i.e. bound internally to an
 * Observable element. So far, only the Thread element supports this
 * flag.
 */
#define EVL_CLONE_OBSERVABLE	(1 << 17)
/*
 * Request that O_NONBLOCK be set on the file descriptor of a newly
 * created element. This should be handled by the interface library
 * issuing the EVL_IOC_CLONE command to the control device.
 */
#define EVL_CLONE_NONBLOCK	(1 << 18)
/*
 * Observable-specific. Request that each notification be sent to a
 * single receiver.
 */
#define EVL_CLONE_UNICAST	(1 << 19)
/*
 * Proxy-specific. Data direction is from proxied file to proxy.
 */
#define EVL_CLONE_INPUT		(1 << 20)
/*
 * Proxy-specific. Data direction is from proxy to proxied file.
 */
#define EVL_CLONE_OUTPUT	(1 << 21)
/*
 * Enable shareability of element in fundle-based operations. If
 * unset, a fundle-based lookup of such element can succeed only in
 * the context of the process which created it. Applicable to Thread,
 * Monitor and Observable elements.
 */
#define EVL_CLONE_SHAREABLE	(1 << 22)
/*
 * Element was created by the core or some kernel context by
 * extension. Such element may be either public or private. This is
 * used as a discriminator to skip user-oriented setup operations.
 */
#define EVL_CLONE_COREDEV	(1 << 31)

/* All the valid bits as far as user is concerned. */
#define EVL_CLONE_MASK		(((__u32)-1 << 16) & ~EVL_CLONE_COREDEV)

/*
 * Deprecated: this is a longstanding misnomer. This flag is really
 * about sending unicast notifications as opposed to broadcasting
 * events to all observers.
 */
#define EVL_CLONE_MASTER	EVL_CLONE_UNICAST

struct evl_clone_req {
	__u64 name_ptr;		/* (const char __user *name) */
	__u64 attrs_ptr;	/* (void __user *attrs) */
	__u32 clone_flags;
	/* Output on success: */
	struct evl_element_ids eids;
	__u32 efd;
};

#define EVL_IOC_CLONE	_IOWR(EVL_FACTORY_IOCBASE, 0, struct evl_clone_req)

#endif /* !_EVL_UAPI_FACTORY_ABI_H */
