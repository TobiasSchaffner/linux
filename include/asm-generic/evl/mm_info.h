/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_EVL_MM_INFO_H
#define _ASM_GENERIC_EVL_MM_INFO_H

#include <linux/list.h>
#include <evl/wait.h>

#define EVL_MM_ACTIVE_BIT  30
#define EVL_MM_INIT_BIT    31

struct oob_mm_state {
	unsigned long flags;
	struct evl_wait_queue ptrace_wait;
	u32 ptrace_seq;	      /* current stop/start sequence number */
	struct list_head ptrace_queue; /* thread->ptrace_next */
	struct list_head threads; /* thread->next */
	hard_spinlock_t lock;
};

static inline void init_oob_mm_state(struct oob_mm_state *state)
{
	/* Rest of init happens later on for oob threads only. */
	state->flags = 0;
}

#endif /* !_ASM_GENERIC_EVL_MM_INFO_H */
