/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Copyright (C) 2018 Philippe Gerum <rpm@xenomai.org>
 */

#ifndef _EVL_UAPI_SYSCALL_ABI_H
#define _EVL_UAPI_SYSCALL_ABI_H

#define sys_evl_read	0	/* oob_read() */
#define sys_evl_write	1	/* oob_write() */
#define sys_evl_ioctl	2	/* oob_ioctl() */
#define sys_evl_functl	3	/* oob_funcall() */
#define sys_evl_ifunctl	4	/* inband_funcall() */

#define NR_EVL_SYSCALLS 5

#endif /* !_EVL_UAPI_SYSCALL_ABI_H */
