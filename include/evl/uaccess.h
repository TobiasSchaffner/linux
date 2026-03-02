/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_UACCESS_H
#define _EVL_UACCESS_H

#include <linux/uaccess.h>
#include <evl/timeout.h>
#include <asm/evl/syscall.h>
#include <uapi/evl/types-abi.h>

/*
 * A helper struct used in contexts accepting both kernel-originated
 * and user-originated timeout specs.
 */
struct evl_timespec_union {
	union {
		struct __evl_timespec __user *u_timespec;
		ktime_t kt;
	};
	int abstime : 1;
	int user : 1;
};

static inline unsigned long __must_check
raw_copy_from_user_ptr64(void *to, u64 from_ptr, unsigned long n)
{
	return raw_copy_from_user(to, (void *)(long)from_ptr, n);
}

static inline unsigned long __must_check
raw_copy_to_user_ptr64(u64 to, const void *from, unsigned long n)
{
	return raw_copy_to_user((void *)(long)to, from, n);
}

#define evl_ptrval64(__ptr)		((u64)(long)(__ptr))
#define evl_valptr64(__ptrval, __type)	((__type *)(long)(__ptrval))

int evl_fetch_utimespec(struct __evl_timespec __user *u_ts,
			ktime_t *timeout, enum evl_tmode *tmode);

#endif /* !_EVL_UACCESS_H */
