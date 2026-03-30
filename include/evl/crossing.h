/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_CROSSING_H
#define _EVL_CROSSING_H

#include <linux/refcount.h>
#include <linux/completion.h>
#include <linux/irq_work.h>

/*
 * This is a simple synchronization mechanism allowing an in-band
 * caller to pass a point in the code making sure that no concurrent
 * out-of-band operation which might traverse the same crossing is in
 * flight.
 *
 * Out-of-band callers delimit the danger zone by down-ing
 * (evl_down_crossing()) and up-ing (evl_up_crossing()) the barrier at
 * the crossing. Conversely, the in-band code should ask for passing
 * the latter (evl_pass_crossing()) to make sure no out-of-band
 * operation is ongoing on the protected resource.
 *
 * CAUTION: the sequence evl_pass_crossing() -> evl_down_crossing() is
 * _invalid_, the implementation must guarantee that it never happens.
 * A call to evl_reinit_crossing() must reset the crossing in-between:
 * evl_pass_crossing() -> evl_reinit_crossing() -> evl_down_crossing().
 */

struct evl_crossing {
	refcount_t refs;
	struct completion passed;
	struct irq_work irq_work;
};

static inline void evl_open_crossing(struct irq_work *work)
{
	struct evl_crossing *c = container_of(work, struct evl_crossing, irq_work);
	complete(&c->passed);
}

static inline void evl_init_crossing(struct evl_crossing *c)
{
	refcount_set(&c->refs, 1);
	init_completion(&c->passed);
	init_irq_work(&c->irq_work, evl_open_crossing);
}

static inline void evl_reinit_crossing(struct evl_crossing *c)
{
	refcount_set(&c->refs, 1);
	reinit_completion(&c->passed);
}

static inline void evl_down_crossing(struct evl_crossing *c)
{
	refcount_inc(&c->refs);
}

static inline void evl_up_crossing(struct evl_crossing *c)
{
	/* CAUTION: See word of caution in the initial comment. */

	if (refcount_dec_and_test(&c->refs)) {
		if (running_inband() && !hard_irqs_disabled())
			complete(&c->passed);
		else
			irq_work_queue(&c->irq_work);
	}
}

static inline void evl_pass_crossing(struct evl_crossing *c)
{
	if (!refcount_dec_and_test(&c->refs))
		wait_for_completion(&c->passed);
}

#endif /* !_EVL_CROSSING_H */
