/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_SKBUFF_H
#define _EVL_DOVETAIL_SKBUFF_H

#ifdef CONFIG_NET_OOB
#include <asm-generic/evl/skbuff.h>
#else
#include_next <dovetail/skbuff.h>
#endif

#endif /* !_EVL_DOVETAIL_SKBUFF_H */
