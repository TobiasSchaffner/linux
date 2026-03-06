/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_EVL_MM_INFO_H
#define _ASM_GENERIC_EVL_MM_INFO_H

#include <linux/list.h>
#include <evl/wait.h>

#define EVL_MM_ACTIVE_BIT  30
#define EVL_MM_INIT_BIT    31

/*
 * A scope is merely a placeholder designating the context which
 * defines it.
 */
struct evl_scope { };

struct oob_mm_state {
	/* EVL_MM_*_BIT */
	unsigned long flags;
	/* Ptrace wait barrier. */
	struct evl_wait_queue ptrace_wait;
	/* Current ptracing stop/start sequence number. */
	u32 ptrace_seq;
	/* List of waiting ptrace-stopped threads. */
	struct list_head ptrace_queue;
	/* Elements whose lifetime is bound to the process. */
	struct list_head elements;
	/* List of (EVL) threads attached to process. */
	struct list_head threads;
	/* Guards ptrace_queue, threads, elements. */
	hard_spinlock_t lock;
	/* Scope of process-private elements. */
	struct evl_scope scope;
};

static inline void init_oob_mm_state(struct oob_mm_state *state)
{
	/*
	 * Rest of init may be performed by evl_activate_oob_mm()
	 * later on, only for threads bound to the core.
	 */
	state->flags = 0;
}

#endif /* !_ASM_GENERIC_EVL_MM_INFO_H */
