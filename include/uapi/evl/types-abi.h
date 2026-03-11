/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Derived from Xenomai Cobalt, https://xenomai.org/
 * Copyright (C) 2013-2026 Philippe Gerum <rpm@xenomai.org>
 */

#ifndef _EVL_UAPI_TYPES_ABI_H
#define _EVL_UAPI_TYPES_ABI_H

#include <linux/types.h>

typedef __u32 fundle_t;

#define EVL_NO_HANDLE	0

/* Reserved (high) bits in fundle (31-28). */
#define __FUNDLE_CLAIMED_BITS	1
#define __FUNDLE_CLAIMED_SHIFT	31
#define __FUNDLE_CEILING_BITS	1
#define __FUNDLE_CEILING_SHIFT	30
#define __FUNDLE_TYPE_BITS	2
#define __FUNDLE_TYPE_SHIFT	28
#define __FUNDLE_MASK(x)	((1U << (x)) - 1)

#define __FUNDLE_CLAIMED_MASK	(__FUNDLE_MASK(__FUNDLE_CLAIMED_BITS) << __FUNDLE_CLAIMED_SHIFT)
#define __FUNDLE_CEILING_MASK	(__FUNDLE_MASK(__FUNDLE_CEILING_BITS) << __FUNDLE_CEILING_SHIFT)
#define __FUNDLE_TYPE_MASK	(__FUNDLE_MASK(__FUNDLE_TYPE_BITS) << __FUNDLE_TYPE_SHIFT)
#define __FUNDLE_KEY_MASK	(~(__FUNDLE_CLAIMED_MASK|__FUNDLE_CEILING_MASK))

/*
 * Strip the runtime information from the fundle, only retaining its
 * key value in the map.
 */
static inline fundle_t __evl_fundle_key(fundle_t handle)
{
	return handle & __FUNDLE_KEY_MASK;
}

/* Extract the type of the element from the fundle. */
static inline int __evl_fundle_type(fundle_t handle)
{
	return (int)((handle & __FUNDLE_TYPE_MASK) >> __FUNDLE_TYPE_SHIFT);
}

/*
 * Y2038 safety. Match the kernel ABI definitions of __kernel_timespec
 * and __kernel_itimerspec.
 */
typedef long long __evl_time64_t;

struct __evl_timespec {
	__evl_time64_t tv_sec;
	long long      tv_nsec;
};

struct __evl_itimerspec {
	struct __evl_timespec it_interval;
	struct __evl_timespec it_value;
};

union evl_value {
	__s32 val;
	__s64 lval;
	void *ptr;
};

#define evl_intval(__val)	((union evl_value){ .lval = (__val) })
#define evl_ptrval(__ptr)	((union evl_value){ .ptr = (__ptr) })
#define evl_nil			evl_intval(0)

/*
 * All shared states of elements for which fundle-based access is
 * provided must start with the following header.
 */
struct __evl_sstate_header {
	fundle_t fundle;
};

#endif /* !_EVL_UAPI_TYPES_ABI_H */
