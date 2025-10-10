/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_POLL_H
#define _EVL_DOVETAIL_POLL_H

#ifdef CONFIG_EVL
#include <asm-generic/evl/poll.h>
#else
#include_next <dovetail/poll.h>
#endif

#endif /* !_EVL_DOVETAIL_POLL_H */
