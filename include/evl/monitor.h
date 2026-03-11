/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_MONITOR_H
#define _EVL_MONITOR_H

#include <linux/list.h>
#include <linux/wait.h>
#include <linux/irq_work.h>
#include <evl/wait.h>
#include <evl/thread.h>
#include <evl/mutex.h>
#include <evl/poll.h>
#include <evl/factory.h>
#include <uapi/evl/monitor-abi.h>

struct evl_monitor {
	struct evl_element element;
	struct __evl_monitor_sstate *sstate;
	int type : 2,
	    protocol : 4;
	union {
		struct {
			struct evl_mutex mutex;
			struct list_head events;
			hard_spinlock_t lock;
		};
		struct {
			/* Out-of-band wait queue. */
			struct evl_wait_queue wait_queue;
			/* In-band wait queue (read side). */
			wait_queue_head_t inband_wait_r;
			/* In-band wait queue (write side). */
			wait_queue_head_t inband_wait_w;
			/* In-band wake up work (read side). */
			struct irq_work inband_wake_r;
			/* In-band wake up work (write side). */
			struct irq_work inband_wake_w;
			/* Gate (valid during active wait only). */
			struct evl_monitor *gate;
			/* Out-of-band poll head. */
			struct evl_poll_head poll_head;
			/* in ->events */
			struct list_head next;
		};
	};
};

long evl_functl_monitor(struct evl_monitor *mon,
		unsigned int cmd, unsigned long arg);

int evl_signal_monitor_targeted(struct evl_thread *target,
				int monfd);

void __evl_commit_monitor_ceiling(void);

static inline void evl_commit_monitor_ceiling(void)
{
	struct evl_thread *curr = evl_current();

	if (curr->sstate->pp_pending != EVL_NO_HANDLE)
		__evl_commit_monitor_ceiling();
}

#endif /* !_EVL_MONITOR_H */
