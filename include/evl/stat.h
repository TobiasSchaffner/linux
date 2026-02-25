/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Derived from Xenomai Cobalt, https://xenomai.org/
 * Copyright (C) 2006 Jan Kiszka <jan.kiszka@web.de>.
 * Copyright (C) 2006 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 * Copyright (C) 2008, 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_STAT_H
#define _EVL_STAT_H

#include <evl/assert.h>
#include <evl/clock.h>

struct evl_rq;

typedef struct {
	u64 value;
} evl_counter64;

typedef struct {
	u32 value;
} evl_counter32;

#define evl_counter_inc(__c)		(++(__c)->value)
#define evl_counter_read(__c)		((__c)->value)
#define evl_counter_set(__c, __v)	((__c)->value = (__v))
#define evl_counter_add(__c, __n)	((__c)->value += (__n))

/*
 * The careful helpers won't guarantee atomic updates of the counter,
 * but do prevent from load/store tearing and other sorts of
 * compiler-originated shenanigans.
 */
#define evl_counter_set_careful(__c, __v)	WRITE_ONCE((__c)->value, (__v))
#define evl_counter_read_careful(__c)		READ_ONCE((__c)->value)
#define evl_counter_add_careful(__c, __n)			\
	do {							\
		typeof((__c)->value) ___oldv =			\
			evl_counter_read_careful(__c);		\
		WRITE_ONCE((__c)->value, ___oldv + __n);	\
	} while (0)
#define evl_counter_inc_careful(__c)		evl_counter_add_careful(__c, 1)

#ifdef CONFIG_EVL_RUNSTATS

struct evl_account {
	ktime_t start;   /* Start of execution time accumulation */
	ktime_t total;	 /* Accumulated execution time */
};

/*
 * Return current date which can be passed to other accounting
 * services. We do not use sched_clock() on purpose: its worst case
 * execution time may be really bad under some combination of clock
 * data updates and high cache pressure.
 */
static inline ktime_t evl_get_timestamp(void)
{
	return evl_read_clock(&evl_mono_clock);
}

static inline ktime_t evl_get_account_total(struct evl_account *account)
{
	return account->total;
}

/* Reset accounting, e.g. after CPU migration. */
static inline void evl_reset_account(struct evl_account *account)
{
	account->total = 0;
	account->start = evl_get_timestamp();
}

#define evl_set_current_account(__rq, __new_account)			\
	do {								\
		(__rq)->current_account = (__new_account);		\
	} while (0)

/*
 * Accumulate the time spent on the current account then switch to a
 * different account.
 */
#define evl_switch_account(__rq, __new_account)			\
	do {							\
		assert_hard_lock(&(__rq)->lock);		\
		ktime_t __now = evl_get_timestamp();		\
		(__rq)->current_account->total +=		\
			__now - (__rq)->last_account_switch;	\
		(__rq)->last_account_switch = __now;		\
		smp_wmb();					\
		evl_set_current_account(__rq, __new_account);	\
	} while (0)

struct evl_opt_counter {
	unsigned long value;
};

#define evl_opt_counter_set(__c, __value)	evl_counter_set_careful(__c, __value)
#define evl_opt_counter_inc(__c)		evl_counter_inc_careful(__c)
#define evl_opt_counter_read(__c)		evl_counter_read_careful(__c)

#else /* !CONFIG_EVL_RUNSTATS */

struct evl_account {
};

#define evl_get_timestamp()				({ 0; })
#define evl_get_account_total(__account)		({ 0; })
#define evl_reset_account(__account)			do { } while (0)
#define evl_set_current_account(__rq, __new_account)	do { assert_hard_lock(&(__rq)->lock); (void)(__new_account); } while (0)
#define evl_switch_account(__rq, __new_account)		evl_set_current_account(__rq, __new_account)

struct evl_opt_counter {
};

#define evl_opt_counter_inc(__c) 		({ do { } while(0); 0; })
#define evl_opt_counter_read(__c) 		({ 0; })
#define evl_opt_counter_set(__c, __value)	do { } while (0)

#endif /* CONFIG_EVL_RUNSTATS */

#endif /* !_EVL_STAT_H */
