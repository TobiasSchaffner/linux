/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/export.h>
#include <linux/uaccess.h>
#include <evl/clock.h>
#include <evl/uaccess.h>

int evl_fetch_utimespec(struct __evl_timespec __user *u_ts,
			ktime_t *timeout, enum evl_tmode *tmode)
{
	struct __evl_timespec uts;
	struct timespec64 ts64;
	int ret;

	ret = raw_copy_from_user(&uts, u_ts, sizeof(uts));
	if (ret)
		return -EFAULT;

	if ((unsigned long)uts.tv_nsec >= ONE_BILLION)
		return -EINVAL;

	ts64 = u_timespec_to_timespec64(uts);
	*timeout = timespec64_to_ktime(ts64);
	*tmode = *timeout ? EVL_ABS : EVL_REL;

	return 0;
}
EXPORT_SYMBOL_GPL(evl_fetch_utimespec);
