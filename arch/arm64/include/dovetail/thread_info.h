/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_THREAD_INFO_H
#define _EVL_DOVETAIL_THREAD_INFO_H

#ifdef CONFIG_EVL
#include <asm-generic/evl/thread_info.h>
#else
#include_next <dovetail/thread_info.h>
#endif

#endif /* !_EVL_DOVETAIL_THREAD_INFO_H */
