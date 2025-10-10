/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_NET_H
#define _EVL_DOVETAIL_NET_H

#ifdef CONFIG_NET_OOB
#include <asm-generic/evl/net.h>
#else
#include_next <dovetail/net.h>
#endif

#endif /* !_EVL_DOVETAIL_NET_H */
