/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2023 Philippe Gerum  <rpm@xenomai.org>
 *
 * Dual stage RCU-aware list helpers which allow for in-band updates
 * and out-of-band lookups.
 */

#ifndef _EVL_LIB_RCULIST_H
#define _EVL_LIB_RCULIST_H

#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>

struct evl_rculist {
	/* For serializing updates (in-band only). */
	spinlock_t lock;
	/* List of entries. */
	struct list_head list;
};

static inline void evl_init_rculist(struct evl_rculist *rculist)
{
	spin_lock_init(&rculist->lock);
	INIT_LIST_HEAD_RCU(&rculist->list);
}

/*
 * We serialize with inband tasks and BH, not IRQ handlers which
 * should never mutate an rculist.
 */
static inline void evl_lock_rculist(struct evl_rculist *rculist)
{
	spin_lock_bh(&rculist->lock);
}

static inline void evl_unlock_rculist(struct evl_rculist *rculist)
{
	spin_unlock_bh(&rculist->lock);
}

/*
 * Enqueue a new entry into a list which must have been locked prior
 * to calling this routine. This call must be issued from the in-band
 * stage.
 *
 * @entry entry to add to the list.
 */
static inline
void evl_add_rculist_entry_locked(struct evl_rculist *rculist, /* in-band */
				struct list_head *entry)
{
	list_add_tail_rcu(entry, &rculist->list);
}

/*
 * Enqueue a new entry into a list. This call must be issued from the
 * in-band stage.
 *
 * @entry entry to add to the list.
 */
static inline
void evl_add_rculist_entry(struct evl_rculist *rculist, /* in-band */
			struct list_head *entry)
{
	spin_lock_bh(&rculist->lock);
	evl_add_rculist_entry_locked(rculist, entry);
	spin_unlock_bh(&rculist->lock);
}

/*
 * Remove an entry from a list, which must have been locked prior to
 * calling this routine. This call must be issued from the in-band
 * stage.
 *
 * @entry entry to remove from the list.
 */
static inline
void evl_del_rculist_entry_locked(struct list_head *entry) /* in-band */
{
	list_del_rcu(entry);
}

/*
 * Remove an entry from the list. This call must be issued from the
 * in-band stage.
 *
 * @entry entry to remove from the list.
 */
static inline
void evl_del_rculist_entry(struct evl_rculist *rculist,
			   struct list_head *entry) /* in-band */
{
	spin_lock_bh(&rculist->lock);
	evl_del_rculist_entry_locked(entry);
	spin_unlock_bh(&rculist->lock);
}

/* RCU read-side lock must be held. */
#define evl_rculist_for_each_entry(__pos, __head, __member)	\
	list_for_each_entry_rcu(__pos, &(__head)->list, __member)

#define evl_rculist_empty(__head)	list_empty(&(__head)->list)

#endif /* !_EVL_LIB_RCULIST_H */
