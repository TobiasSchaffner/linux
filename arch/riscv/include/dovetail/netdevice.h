/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_NETDEVICE_H
#define _EVL_DOVETAIL_NETDEVICE_H

#ifdef CONFIG_NET_OOB
#include <asm-generic/evl/netdevice.h>
#else
#include_next <dovetail/netdevice.h>
#endif

#endif /* !_EVL_DOVETAIL_NETDEVICE_H */
