/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_EVL_MM_INFO_H
#define _ASM_GENERIC_EVL_MM_INFO_H

#include <linux/list.h>
#include <evl/wait.h>

#define EVL_MM_PTSYNC_BIT  0
#define EVL_MM_ACTIVE_BIT  30
#define EVL_MM_INIT_BIT    31

struct evl_wait_queue;

struct oob_mm_state {
	unsigned long flags;	/* Guaranteed zero initially. */
	struct list_head ptrace_sync;
	struct evl_wait_queue ptsync_barrier;
};

static inline void init_oob_mm_state(struct oob_mm_state *state)
{
	/* Actual init happens later on for oob threads only. */
	memset(state, 0, sizeof(*state));
}

#endif /* !_ASM_GENERIC_EVL_MM_INFO_H */
