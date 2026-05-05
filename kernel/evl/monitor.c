/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/slab.h>
#include <linux/compat.h>
#include <evl/memory.h>
#include <evl/monitor.h>
#include <evl/namespace.h>
#include <evl/uaccess.h>
#include <trace/events/evl.h>

/* Valid clone flags for a monitor element. */
#define EVL_MONITOR_CLONE_FLAGS	(EVL_CLONE_PUBLIC|EVL_CLONE_SHAREABLE)

static __always_inline  atomic_t *__ATOMIC32(__u32 *ptr)
{
	return (atomic_t *)ptr;
}

static struct evl_monitor *get_monitor_by_fundle(fundle_t fundle, int type)
{
	struct evl_monitor *mon = evl_lookup_ns(&evl_core_ns, fundle, monitor);

	if (mon && mon->type != type) {
		evl_put_element(&mon->element);
		return NULL;
	}

	return mon;
}

int evl_signal_monitor_targeted(struct evl_thread *target, __u32 eventfun)
{
	struct evl_wait_channel *wchan;
	struct evl_monitor *event;
	unsigned long flags;
	int ret = 0;

	event = get_monitor_by_fundle(eventfun, EVL_MONITOR_EVENT);
	if (event == NULL)
		return -EINVAL;

	/*
	 * Current must hold the gate lock before calling us; if not,
	 * we might race updating sstate->flags, possibly loosing
	 * events. Too bad.
	 */
	raw_spin_lock_irqsave(&target->lock, flags);

	wchan = evl_get_thread_wchan(target);
	if (wchan == &event->wait_queue.wchan) {
		event->sstate->flags.targeted = true;
		event->sstate->flags.signaled = true;
		raw_spin_lock(&target->rq->lock);
		target->info |= EVL_T_SIGNAL;
		raw_spin_unlock(&target->rq->lock);
	}

	if (wchan)
		evl_put_thread_wchan(wchan);

	raw_spin_unlock_irqrestore(&target->lock, flags);

	evl_put_element(&event->element);

	return ret;
}

void __evl_commit_monitor_ceiling(void)
{
	struct evl_thread *curr = evl_current();
	struct evl_monitor *gate;

	/*
	 * curr->sstate has to be valid since curr bears EVL_T_USER.  If
	 * pp_pending is a bad handle, just skip ceiling.
	 */
	gate = evl_lookup_ns(&evl_core_ns, curr->sstate->pp_pending, monitor);
	if (IS_ERR_OR_NULL(gate))
		goto out;

	if (gate->protocol == EVL_GATE_PP)
		evl_commit_mutex_ceiling(&gate->mutex);

	evl_put_element(&gate->element);
out:
	curr->sstate->pp_pending = EVL_NO_HANDLE;
}

/* gate->lock + event->wait_queue.wchan.lock held, irqs off */
static void untrack_event(struct evl_monitor *event, struct evl_monitor *gate)
{
	assert_hard_lock(&gate->lock);
	assert_hard_lock(&event->wait_queue.wchan.lock);

	/*
	 * If no more waiter is pending on this event, have the gate
	 * stop tracking it.
	 */
	if (event->gate == gate && !evl_wait_active(&event->wait_queue)) {
		list_del(&event->next);
		event->gate = NULL;
		event->sstate->u.event.gate_offset = EVL_MONITOR_NOGATE;
	}
}

/* gate->lock + event->wait_queue.wchan.lock held, irqs off */
static void wakeup_waiters(struct evl_monitor *event, struct evl_monitor *gate)
{
	struct __evl_monitor_sstate *sstate = event->sstate;
	bool bcast = sstate->flags.broadcast;
	struct evl_thread *waiter, *n;

	/*
	 * We are called upon exiting a gate which serializes access
	 * to a signaled event. Unblock the thread(s) satisfied by the
	 * signal, either all of them, a designated set or the first
	 * waiter in line, depending on whether this is due to a
	 * broadcast, targeted or regular notification.
	 *
	 * Precedence order for event delivery is as follows:
	 * broadcast > targeted > regular.  This means that a
	 * broadcast notification is considered first and applied if
	 * detected. Otherwise, and in presence of a targeted wake up
	 * request, only target threads are resumed. Otherwise, the
	 * thread heading the wait queue is readied.
	 */
	if (evl_wait_active(&event->wait_queue)) {
		if (bcast) {
			evl_flush_wait_locked(&event->wait_queue, 0);
		} else if (sstate->flags.targeted) {
			evl_for_each_waiter_safe(waiter, n, &event->wait_queue) {
				if (waiter->info & EVL_T_SIGNAL)
					evl_wake_up(&event->wait_queue,
						waiter, 0);
			}
		} else {
			evl_wake_up_head(&event->wait_queue);
		}
		untrack_event(event, gate);
	} /* Otherwise, spurious wakeup (fine, might happen). */

	sstate->flags.all = 0;
}

static int __enter_monitor(struct evl_monitor *gate,
			struct __evl_timespec __user *u_timeout)
{
	if (u_timeout)
		return __evl_lock_mutex_timeout(&gate->mutex,
				(struct evl_timespec_union){
				  .u_timespec = u_timeout,
				  .abstime = EVL_ABS,
				  .user = 1
				});
	/* Infinite wait. */
	return __evl_lock_mutex_timeout(&gate->mutex,
			(struct evl_timespec_union){
			  .kt = EVL_INFINITE,
			  .abstime = EVL_REL,
			});
}

static int enter_monitor(struct evl_monitor *gate,
			struct __evl_timespec __user *u_timeout)
{
	struct evl_thread *curr = evl_current();
	int ret;

	if (gate->type != EVL_MONITOR_GATE)
		return -EINVAL;

	/*
	 * We only handle first-order locking, user-space should deal
	 * with locking recursion.
	 */
	if (evl_is_mutex_owner(gate->mutex.fastlock, fundle_of(curr)))
		return -EDEADLK;

	evl_commit_monitor_ceiling();

	ret = __enter_monitor(gate, u_timeout);
	if (ret)
		return ret;

	gate->sstate->u.gate.nesting = 1;

	return ret;
}

static int tryenter_monitor(struct evl_monitor *gate)
{
	int ret;

	if (gate->type != EVL_MONITOR_GATE)
		return -EINVAL;

	evl_commit_monitor_ceiling();

	ret = evl_trylock_mutex(&gate->mutex);
	if (ret)
		return ret;

	gate->sstate->u.gate.nesting = 1;

	return 0;
}

/*
 * __exit_monitor - drops a gate mutex.
 *
 * Called with gate->mutex locked, hard irqs off. If the mutex guards
 * a signaled event, wake up the waiters before dropping the lock. The
 * whole wakeup+exit sequence must appear as atomic.
 */
static void __exit_monitor(struct evl_monitor *gate, struct evl_thread *curr) /* irqs off */
{
	struct __evl_monitor_sstate *sstate = gate->sstate;
	struct evl_monitor *event, *n;

	/*
	 * Since gate->mutex is held, we can manipulate the shared
	 * state flags racelessly.
	 */
	if (sstate->flags.signaled) {
		sstate->flags.signaled = false;
		list_for_each_entry_safe(event, n, &gate->events, next) {
			raw_spin_lock(&event->wait_queue.wchan.lock);
			if (event->sstate->flags.signaled)
				wakeup_waiters(event, gate);
			raw_spin_unlock(&event->wait_queue.wchan.lock);
		}
	}

	/*
	 * If we are about to release the lock which is still pending
	 * PP (i.e. we never got scheduled out while holding it),
	 * clear the lazy handle.
	 */
	if (fundle_of(gate) == curr->sstate->pp_pending)
		curr->sstate->pp_pending = EVL_NO_HANDLE;

	__evl_unlock_mutex(&gate->mutex);
}

static int exit_monitor(struct evl_monitor *gate)
{
	struct evl_thread *curr = evl_current();
	unsigned long flags;

	if (gate->type != EVL_MONITOR_GATE)
		return -EINVAL;

	if (!evl_is_mutex_owner(gate->mutex.fastlock, fundle_of(curr)))
		return -EPERM;

	/*
	 * Locking order is gate lock first, depending event lock(s)
	 * next.
	 */
	raw_spin_lock_irqsave(&gate->lock, flags);

	__exit_monitor(gate, curr);

	raw_spin_unlock_irqrestore(&gate->lock, flags);

	evl_schedule();

	return 0;
}

static int __trywait_count(struct evl_monitor *mon)
{
	struct __evl_monitor_sstate *sstate = mon->sstate;
	int ret = 0, val;

	/* atomic_dec_if_strictly_positive */
	val = atomic_read(__ATOMIC32(&sstate->u.event.value));
	do {
		if (unlikely(val <= 0)) {
			ret = -EAGAIN;
			break;
		}
	} while (!atomic_try_cmpxchg(__ATOMIC32(&sstate->u.event.value), &val, val - 1));

	return ret;
}

/* mon->wait_queue.wchan.lock held, dropped on success, irqs off. */
static int trywait_count_locked(struct evl_monitor *mon, unsigned long flags)
{
	int ret = __trywait_count(mon);

	if (!ret)
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);

	return ret;
}

static int wait_count_oob(struct evl_monitor *mon,
		unsigned int f_flags,
		struct __evl_timespec __user *u_timeout)
{
	struct __evl_monitor_sstate *sstate = mon->sstate;
	ktime_t timeout = EVL_INFINITE;
	enum evl_tmode tmode = EVL_REL;
	unsigned long flags;
	int ret;

	ret = __trywait_count(mon);
	if (!ret || f_flags & O_NONBLOCK)
		return ret;

	if (u_timeout) {
		ret = evl_fetch_utimespec(u_timeout, &timeout, &tmode);
		if (ret)
			return ret;
	}

	/*
	 * CAUTION: we must fully serialize with post_count(). Since
	 * user-space is expected to trywait first before branching
	 * here, we are most likely going to wait anyway.
	 */
	raw_spin_lock_irqsave(&mon->wait_queue.wchan.lock, flags);
	if (atomic_dec_return(__ATOMIC32(&sstate->u.event.value)) >= 0) {
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock,	flags);
		return 0;
	}

	evl_add_wait_queue(&mon->wait_queue, timeout, tmode);
	raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock,	flags);

	ret = evl_wait_schedule(&mon->wait_queue);
	if (ret) { /* Rollback decrement if failed. */
		atomic_inc(__ATOMIC32(&sstate->u.event.value));
	} else {
		/*
		 * If waking up on a broadcast, we did not actually
		 * receive a free pass, make the caller notice by
		 * returning -EAGAIN.
		 */
		if (evl_current()->info & EVL_T_BCAST)
			ret = -EAGAIN;
	}

	return ret;
}

static int post_count(struct evl_monitor *mon, s32 sigval,
		bool bcast)
{
	struct __evl_monitor_sstate *sstate = mon->sstate;
	bool pollable = true;
	unsigned long flags;
	int val;

	/*
	 * We may receive a null sigval for the purpose of triggering
	 * a poll notification without updating the count.
	 *
	 * Otherwise, we can either post a single waiter or broadcast
	 * the semaphore which unblocks all waiters at once, making
	 * them know they were not granted anything in the process
	 * though. Broadcasting here is equivalent to a flush
	 * operation.
	 */
	if (sigval) {
		raw_spin_lock_irqsave(&mon->wait_queue.wchan.lock, flags);

		if (bcast) {
			val = evl_flush_wait_locked(&mon->wait_queue, EVL_T_BCAST);
			/*
			 * Each waiter found sleeping on the wait
			 * queue counts for 1 semaphore unit to be
			 * added back to the count.
			 */
			if (val > 0)
				atomic_add(val, __ATOMIC32(&sstate->u.event.value));
			/* Userland might have slipped in, re-check. */
			pollable = atomic_read(__ATOMIC32(&sstate->u.event.value)) > 0;
		} else {
			if (atomic_inc_return(__ATOMIC32(&sstate->u.event.value)) <= 0) {
				evl_wake_up_head(&mon->wait_queue);
				pollable = false;
			}
		}

		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);
	}

	if (pollable)
		evl_signal_poll_events(&mon->poll_head, POLLIN|POLLRDNORM);

	evl_schedule();

	/* Wake up an in-band waiter last. */
	smp_mb();
	if (waitqueue_active(&mon->inband_wait_r)) {
		if (running_inband()) {
			wake_up(&mon->inband_wait_r);
		} else {
			evl_get_element(&mon->element);
			if (!irq_work_queue(&mon->inband_wake_r))
				evl_put_element(&mon->element);
		}
	}

	return 0;
}

static void inband_wake_r_irqwork(struct irq_work *work) /* in-band */
{
	struct evl_monitor *mon;

	mon = container_of(work, struct evl_monitor, inband_wake_r);
	wake_up(&mon->inband_wait_r);
	evl_put_element(&mon->element);
}

static void inband_wake_w_irqwork(struct irq_work *work) /* in-band */
{
	struct evl_monitor *mon;

	mon = container_of(work, struct evl_monitor, inband_wake_w);
	wake_up(&mon->inband_wait_w);
	evl_put_element(&mon->element);
}

/* mon->wait_queue.wchan.lock held, dropped on success, irqs off. */
static bool __trywait_mask(struct evl_monitor *mon,
			s32 match_value,
			bool exact_match,
			s32 *r_value,
			unsigned long flags)
{
	struct __evl_monitor_sstate *sstate = mon->sstate;
	int testval;

	*r_value = atomic_read(__ATOMIC32(&sstate->u.event.value)) & match_value;
	testval = exact_match ? match_value : *r_value;
	if (*r_value && *r_value == testval) {
		atomic_andnot(*r_value, __ATOMIC32(&sstate->u.event.value));
		testval = atomic_read(__ATOMIC32(&sstate->u.event.value));
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);
		if (!testval) {
			evl_signal_poll_events(&mon->poll_head, POLLOUT|POLLWRNORM);
			evl_schedule();
		}
		return true;
	}

	return false;
}

static int trywait_mask(struct evl_monitor *mon,
			s32 match_value,
			bool exact_match,
			s32 *r_value)
{
	unsigned long flags;

	/*
	 * Waiting for no bit is interpreted as waiting for any/all of
	 * them (depending on exact_match).
	 */
	if (match_value == 0)
		match_value = -1;

	raw_spin_lock_irqsave(&mon->wait_queue.wchan.lock, flags);
	if (!__trywait_mask(mon, match_value, exact_match, r_value, flags)) {
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);
		return -EAGAIN;
	}

	return 0;
}

struct evl_mask_wait {
	/* Value to wait for. */
	int value;
	/* i.e. Conjunctive/AND/ALL match */
	bool exact_match;
};

static int wait_mask_oob(struct evl_monitor *mon,
			unsigned int f_flags,
			struct __evl_timespec __user *u_timeout,
			s32 match_value,
			bool exact_match,
			s32 *r_value)
{
	struct evl_thread *curr = evl_current();
	ktime_t timeout = EVL_INFINITE;
	enum evl_tmode tmode = EVL_REL;
	struct evl_mask_wait w;
	unsigned long flags;
	int ret;

	if (match_value == 0)	/* See trywait_mask(). */
		match_value = -1;
again:
	raw_spin_lock_irqsave(&mon->wait_queue.wchan.lock, flags);

	if (__trywait_mask(mon, match_value, exact_match, r_value, flags))
		return 0;	/* oob lock already dropped on success. */

	if (f_flags & O_NONBLOCK) {
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);
		return -EAGAIN;
	}

	/*
	 * Fetch the timeout specs since we need it eventually, then
	 * retry once more.
	 */
	if (u_timeout) {
		raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);
		ret = evl_fetch_utimespec(u_timeout, &timeout, &tmode);
		if (ret)
			return ret;
		u_timeout = NULL;
		goto again;
	}

	w.exact_match = exact_match;
	w.value = match_value;
	curr->wait_data = &w;
	evl_add_wait_queue(&mon->wait_queue, timeout, tmode);

	raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);

	ret = evl_wait_schedule(&mon->wait_queue);
	if (!ret)
		*r_value = w.value;

	return ret;
}

static int post_mask(struct evl_monitor *mon, int bits, bool bcast)
{
	struct __evl_monitor_sstate *sstate = mon->sstate;
	int waitval, testval, consumed = 0, val;
	struct evl_thread *waiter, *tmp;
	struct evl_mask_wait *w;
	unsigned long flags;

	raw_spin_lock_irqsave(&mon->wait_queue.wchan.lock, flags);

	/*
	 * NOTE: the only reasons we still have sstate->u.event.value
	 * as an atomic value ATM is strictly for ABI preservation,
	 * and allow for peeking at the mask value directly from
	 * userland (which is hardly a common operation and could be
	 * done differently anyway). We may turn this into a plain
	 * word when an ABI jump is required from applications for
	 * some compelling reason.
	 */
	atomic_or(bits, __ATOMIC32(&sstate->u.event.value));

	evl_for_each_waiter_safe(waiter, tmp, &mon->wait_queue) {
		w = waiter->wait_data;
		waitval = w->value & atomic_read(__ATOMIC32(&sstate->u.event.value));
		testval = w->exact_match ? w->value : waitval;
		if (waitval && waitval == testval) {
			w->value = waitval;
			consumed |= waitval;
			evl_wake_up(&mon->wait_queue, waiter, 0);
			if (!bcast)
				break;
		}
	}

	if (consumed)
		atomic_andnot(consumed, __ATOMIC32(&sstate->u.event.value));

	val = atomic_read(__ATOMIC32(&sstate->u.event.value));

	raw_spin_unlock_irqrestore(&mon->wait_queue.wchan.lock, flags);

	evl_signal_poll_events(&mon->poll_head,
			val ? POLLIN|POLLRDNORM : POLLOUT|POLLWRNORM);

	evl_schedule();

	/*
	 * Now wake up any in-band waiter if we still have events to
	 * consume.
	 */
	smp_mb();
	if (val && waitqueue_active(&mon->inband_wait_r)) {
		if (running_inband()) {
			wake_up(&mon->inband_wait_r);
		} else {
			evl_get_element(&mon->element);
			if (!irq_work_queue(&mon->inband_wake_r))
				evl_put_element(&mon->element);
		}
	}

	return 0;
}

static int wait_gated_event(struct evl_monitor *event,
			struct evl_monitor_waitreq *req,
			struct __evl_timespec __user *u_timeout)
{
	struct evl_thread *curr = evl_current();
	struct evl_monitor *gate;
	enum evl_tmode tmode;
	unsigned long flags;
	struct evl_rq *rq;
	ktime_t timeout;
	int ret = 0;

	if (event->protocol != EVL_EVENT_GATED)
		return -EINVAL;

	/* Find the gate monitor protecting us. */
	gate = get_monitor_by_fundle(req->gatefun, EVL_MONITOR_GATE);
	if (gate == NULL)
		return -EINVAL;

	/* Make sure we actually passed the gate. */
	if (!evl_is_mutex_owner(gate->mutex.fastlock, fundle_of(curr))) {
		ret = -EPERM;
		goto put;
	}

	ret = evl_fetch_utimespec(u_timeout, &timeout, &tmode);
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&gate->lock, flags);

	/*
	 * Track event monitors the gate protects. When multiple
	 * threads issue concurrent wait requests on the same event
	 * monitor, they must use the same gate to serialize. Don't
	 * trust userland for maintaining sane tracking info in
	 * gate_offset, keep event->gate on the kernel side for this.
	 */
	if (event->gate == NULL) {
		list_add_tail(&event->next, &gate->events);
		event->gate = gate;
		event->sstate->u.event.gate_offset = evl_shared_offset(gate->sstate);
	} else if (event->gate != gate) {
		raw_spin_unlock_irqrestore(&gate->lock, flags);
		ret = -EBADFD;
		goto put;
	}

	/*
	 * Since we still hold the mutex until __exit_monitor() is
	 * called later on, do not perform the WOLI checks when
	 * enqueuing.
	 */
	raw_spin_lock(&event->wait_queue.wchan.lock);
	evl_add_wait_queue_unchecked(&event->wait_queue, timeout, tmode);
	raw_spin_unlock(&event->wait_queue.wchan.lock);

	rq = evl_get_thread_rq_noirq(curr);
	curr->info &= ~EVL_T_SIGNAL;
	evl_put_thread_rq_noirq(curr, rq);

	__exit_monitor(gate, curr); /* See comment in exit_monitor(). */

	raw_spin_unlock_irqrestore(&gate->lock, flags);

	/*
	 * Wait on the event proper. If any error is received, do not
	 * attempt to reacquire the gate lock just yet as this might
	 * block indefinitely (in theory), and we want any request for
	 * switching to in-band context to be honored asap. The user
	 * should issue MONIOC_UNWAIT in order to grab the gate lock
	 * back whenever it makes sense.
	 */
	ret = evl_wait_schedule(&event->wait_queue);
	if (ret) {
		raw_spin_lock_irqsave(&gate->lock, flags);
		raw_spin_lock(&event->wait_queue.wchan.lock);
		untrack_event(event, gate);
		raw_spin_unlock(&event->wait_queue.wchan.lock);
		raw_spin_unlock_irqrestore(&gate->lock, flags);
	} else {
		ret = __enter_monitor(gate, NULL);
	}

	if (ret == -ERESTARTSYS)
		ret = -EINTR;	/* Prevent syscall restart. */
put:
	evl_put_element(&gate->element);

	return ret;
}

static int wait_monitor_oob(struct evl_monitor *mon,
			unsigned int f_flags,
			struct evl_monitor_waitreq *req,
			struct __evl_timespec __user *u_timeout,
			s32 *r_value,
			bool exact_match)
{
	int ret;

	if (mon->type != EVL_MONITOR_EVENT)
		return -EINVAL;

	if (req->gatefun == EVL_NO_HANDLE) {
		switch (mon->protocol) {
		case EVL_EVENT_COUNT:
			ret = wait_count_oob(mon, f_flags, u_timeout);
			break;
		case EVL_EVENT_MASK:
			ret = wait_mask_oob(mon, f_flags, u_timeout, req->value,
					exact_match, r_value);
			break;
		default:
			ret = -EINVAL;
		}
		return ret;
	}

	return wait_gated_event(mon, req, u_timeout);
}

static int unwait_monitor_oob(struct evl_monitor *mon,
			struct evl_monitor_unwaitreq *req)
{
	struct evl_monitor *gate;
	int ret;

	if (mon->type != EVL_MONITOR_EVENT)
		return -EINVAL;

	/* Find the gate monitor we need to re-acquire. */
	gate = get_monitor_by_fundle(req->gatefun, EVL_MONITOR_GATE);
	if (gate == NULL)
		return -EINVAL;

	ret = enter_monitor(gate, NULL);

	evl_put_element(&gate->element);

	return ret;
}

#define __wait_monitor_inband(__mon, __trywait_ok)					\
	({										\
		__label__ __out;							\
		struct evl_wait_channel *wchan = &(__mon)->wait_queue.wchan; 		\
		wait_queue_head_t *wq = &(__mon)->inband_wait_r;			\
		struct wait_queue_entry wq_entry;					\
		unsigned long flags, ib_flags;						\
		long jiffies = 0;							\
		int ret = 0;								\
											\
		init_wait_entry(&wq_entry, 0);						\
											\
		for (;;) {								\
			spin_lock_irqsave(&wq->lock, ib_flags); 			\
											\
			raw_spin_lock_irqsave(&wchan->lock, flags); 			\
											\
			if (__trywait_ok)						\
				goto __out;						\
											\
			if (f_flags & O_NONBLOCK) {					\
				raw_spin_unlock_irqrestore(&wchan->lock, flags); 	\
				ret = -EAGAIN;						\
				goto __out;						\
			}								\
											\
			/* Wchan locked to serialize with post_{mask, count}(). */ 	\
			if (list_empty(&wq_entry.entry))				\
				__add_wait_queue(&(__mon)->inband_wait_r, &wq_entry);	\
											\
			raw_spin_unlock_irqrestore(&wchan->lock, flags); 		\
											\
			spin_unlock_irqrestore(&wq->lock, ib_flags); 			\
											\
			/* If we need to sleep, try fetching the (absolute) timeout. */	\
			if (u_timeout) {						\
				ret = evl_fetch_utimespec_to_jiffies(u_timeout, &jiffies); \
				if (ret)						\
					break;						\
				u_timeout = NULL;					\
			}								\
											\
			set_current_state(TASK_INTERRUPTIBLE);				\
											\
			if (jiffies) {							\
				jiffies = schedule_timeout(jiffies);			\
				if (!jiffies) {						\
					ret = -ETIMEDOUT;				\
					break;						\
				}							\
			} else {							\
				schedule();						\
			}								\
											\
			if (signal_pending(current)) {					\
				ret = -ERESTARTSYS;					\
				break;							\
			}								\
		}									\
											\
		spin_lock_irqsave(&wq->lock, ib_flags);					\
	__out:										\
		if (!list_empty(&wq_entry.entry))					\
			list_del(&wq_entry.entry);					\
											\
		spin_unlock_irqrestore(&wq->lock, ib_flags);				\
		ret;									\
	})

static int wait_monitor_inband(struct evl_monitor *mon,
			unsigned int f_flags,
			struct __evl_timespec __user *u_timeout,
			s32 *r_value, /* I/O parameter */
			bool exact_match)
{
	s32 match_value = *r_value;
	int ret;

	if (mon->type != EVL_MONITOR_EVENT)
		return -EINVAL;

	switch (mon->protocol) {
	case EVL_EVENT_MASK:
		ret = __wait_monitor_inband(mon, __trywait_mask(mon, match_value, false, r_value, flags));
		/*
		 * If we have successfully collected all the bits,
		 * send a POLLOUT wakeup.
		 */
		smp_mb();	/* Matches mb in set_current_state() */
		if (!ret && waitqueue_active(&mon->inband_wait_w))
			wake_up(&mon->inband_wait_w);
		break;
	case EVL_EVENT_COUNT:
		/*
		 * We are going to run a polling loop with a sleeping
		 * point. Bumping pollrefs forces userland to jump to
		 * the kernel for signaling the semaphore so that we
		 * won't miss any wakeup.
		 */
		atomic_inc(__ATOMIC32(&mon->sstate->u.event.pollrefs));
		ret = __wait_monitor_inband(mon, !trywait_count_locked(mon, flags));
		atomic_dec(__ATOMIC32(&mon->sstate->u.event.pollrefs));
		if (!ret)
			*r_value = 1;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static long __monitor_common_ioctl(struct evl_monitor *mon, unsigned int f_flags,
				unsigned int cmd, unsigned long arg)
{
	struct evl_monitor_trywaitreq twreq, __user *u_twreq;
	struct evl_monitor_waitreq wreq, __user *u_wreq;
	bool bcast = false, exact_match = false;
	struct __evl_timespec __user *u_uts;
	__s32 value;
	int ret;

	if (mon->type != EVL_MONITOR_EVENT)
		return -EINVAL;

	switch (cmd) {
	case EVL_MONIOC_WAIT_EXACT:
		exact_match = true;
		fallthrough;
	case EVL_MONIOC_WAIT:
		u_wreq = (typeof(u_wreq))arg;
		ret = raw_copy_from_user(&wreq, u_wreq, sizeof(wreq));
		if (ret)
			return -EFAULT;
		u_uts = evl_valptr64(wreq.timeout_ptr, struct __evl_timespec);
		if (running_inband()) {
			if (wreq.gatefun != EVL_NO_HANDLE)
				return -EINVAL;
			value = wreq.value; /* match value. */
			ret = wait_monitor_inband(mon, f_flags, u_uts, &value, exact_match);
		} else {
			ret = wait_monitor_oob(mon, f_flags, &wreq, u_uts, &value, exact_match);
		}
		if (!ret)
			raw_put_user(value, &u_wreq->value);
		break;
	case EVL_MONIOC_TRYWAIT_EXACT:
		exact_match = true;
		fallthrough;
	case EVL_MONIOC_TRYWAIT:
		u_twreq = (typeof(u_twreq))arg;
		ret = raw_copy_from_user(&twreq, u_twreq, sizeof(twreq));
		if (ret)
			return -EFAULT;
		switch (mon->protocol) {
		case EVL_EVENT_COUNT:
			ret = __trywait_count(mon);
			break;
		case EVL_EVENT_MASK:
			ret = trywait_mask(mon, twreq.value, exact_match, &value);
			if (!ret)
				raw_put_user(value, &u_twreq->value);
			break;
		default:
			ret = -EINVAL;
		}
		break;
	case EVL_MONIOC_BROADCAST:
		bcast = true;
		fallthrough;
	case EVL_MONIOC_SIGNAL:
		if (raw_get_user(value, (__s32 __user *)arg))
			return -EFAULT;
		switch (mon->protocol) {
		case EVL_EVENT_COUNT:
			ret = post_count(mon, value, bcast);
			break;
		case EVL_EVENT_MASK:
			ret = post_mask(mon, value, bcast);
			break;
		default:
			ret = -EINVAL;
		}
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static long __monitor_ioctl(struct evl_monitor *mon, unsigned int f_flags,
			unsigned int cmd, unsigned long arg)
{
	struct evl_monitor_binding bind, __user *u_bind;
	long ret;

	switch (cmd) {
	case EVL_MONIOC_BIND:
		bind.type = mon->type;
		bind.protocol = mon->protocol;
		bind.eids.minor = mon->element.minor;
		bind.eids.sstate_offset = evl_shared_offset(mon->sstate);
		bind.eids.fundle = fundle_of(mon);
		u_bind = (typeof(u_bind))arg;
		ret = copy_to_user(u_bind, &bind, sizeof(bind)) ? -EFAULT : 0;
		break;
	default:
		ret = __monitor_common_ioctl(mon, f_flags, cmd, arg);
		break;
	}

	return ret;
}

static long monitor_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);

	return __monitor_ioctl(mon, filp->f_flags, cmd, arg);
}

static long __monitor_oob_ioctl(struct evl_monitor *mon, unsigned int f_flags,
				unsigned int cmd, unsigned long arg)
{
	struct evl_monitor_unwaitreq uwreq, __user *u_uwreq;
	struct __evl_timespec __user *u_uts;
	long ret;

	switch (cmd) {
	case EVL_MONIOC_UNWAIT:
		u_uwreq = (typeof(u_uwreq))arg;
		ret = raw_copy_from_user(&uwreq, u_uwreq, sizeof(uwreq));
		if (ret)
			return -EFAULT;
		ret = unwait_monitor_oob(mon, &uwreq);
		break;
	case EVL_MONIOC_ENTER:
		u_uts = (typeof(u_uts))arg;
		ret = enter_monitor(mon, u_uts);
		break;
	case EVL_MONIOC_TRYENTER:
		ret = tryenter_monitor(mon);
		break;
	case EVL_MONIOC_EXIT:
		ret = exit_monitor(mon);
		break;
	default:
		ret = __monitor_common_ioctl(mon, f_flags, cmd, arg);
	}

	return ret;
}

static long monitor_oob_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);

	return __monitor_oob_ioctl(mon, filp->f_flags, cmd, arg);
}

static void monitor_unwatch(struct evl_poll_head *head)
{
	struct evl_monitor *mon;

	mon = container_of(head, struct evl_monitor, poll_head);
	atomic_dec(__ATOMIC32(&mon->sstate->u.event.pollrefs));
}

static __poll_t monitor_oob_poll(struct file *filp,
				struct oob_poll_wait *wait)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);
	struct __evl_monitor_sstate *sstate = mon->sstate;
	__poll_t ret = 0;
	int val;

	/*
	 * NOTE: for ungated events, we close a race window by queuing
	 * the caller into the poll queue _before_ incrementing the
	 * pollrefs count which userland checks.
	 */
	switch (mon->type) {
	case EVL_MONITOR_EVENT:
		switch (mon->protocol) {
		case EVL_EVENT_COUNT:
			evl_poll_watch(&mon->poll_head, wait, monitor_unwatch);
			atomic_inc(__ATOMIC32(&sstate->u.event.pollrefs));
			if (atomic_read(__ATOMIC32(&sstate->u.event.value)) > 0)
				ret = POLLIN|POLLRDNORM;
			break;
		case EVL_EVENT_MASK:
			evl_poll_watch(&mon->poll_head, wait, monitor_unwatch);
			/*
			 * We need to keep the pollrefs count up to
			 * date as long as we support legacy ABIs
			 * (pre-32).
			 */
			atomic_inc(__ATOMIC32(&sstate->u.event.pollrefs));
			val = atomic_read(__ATOMIC32(&sstate->u.event.value));
			/*
			 * Return POLLIN when some bits are present,
			 * ready for consumption, or POLLOUT when the
			 * mask is entirely clear, i.e. no bit to
			 * consume.
			 */
			if (val)
				ret = POLLIN|POLLRDNORM;
			else
				ret = POLLOUT|POLLWRNORM;
			break;
		case EVL_EVENT_GATED:
			/*
			 * The poll interface does not cope with the
			 * gated event semantics, since we could not
			 * release the gate protecting the event and
			 * enter a poll wait atomically to prevent
			 * missed wakeups.  Therefore, polling a gated
			 * event leads to an error.
			 */
			ret = POLLERR;
			break;
		}
		break;
	case EVL_MONITOR_GATE:
		/*
		 * A mutex should be held only for a short period of
		 * time, with the locked state appearing as a discrete
		 * event to users. Assume a gate lock is always
		 * readable (as "unlocked") then. If this is about
		 * probing for a mutex state from userland then
		 * trylock() should be used instead of poll().
		 */
		ret = POLLIN|POLLRDNORM;
		break;
	}

	return ret;
}

static int monitor_release(struct inode *inode, struct file *filp)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);

	if (mon->type == EVL_MONITOR_EVENT)
		evl_flush_wait(&mon->wait_queue, EVL_T_RMID);
	else
		evl_flush_mutex(&mon->mutex, EVL_T_RMID);

	return evl_release_element(inode, filp);
}

static ssize_t monitor_read(struct file *filp, char __user *u_buf,
			size_t count, loff_t *ppos)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);
	s32 val = -1;		/* Collect any pending event. */

	if (count != sizeof(s32))
		return -EINVAL;

	return wait_monitor_inband(mon, filp->f_flags, NULL, &val, false) ?:
		copy_to_user(u_buf, &val, sizeof(val)) ? -EFAULT : sizeof(val);
}

static ssize_t monitor_common_write(struct file *filp, const char __user *u_buf,
			size_t count)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);
	s32 val;
	int ret;

	if (count != sizeof(s32))
		return -EINVAL;

	if (mon->type != EVL_MONITOR_EVENT)
		return -EINVAL;

	ret = copy_from_user(&val, u_buf, sizeof(val));
	if (ret)
		return -EFAULT;

	switch (mon->protocol) {
	case EVL_EVENT_MASK:
		/* Unicast operation. */
		ret = post_mask(mon, val, false) ?: sizeof(val);
		break;
	case EVL_EVENT_COUNT:
		/* Broadcast if val is zero. */
		ret = post_count(mon, 1, !val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static ssize_t monitor_write(struct file *filp, const char __user *u_buf,
			size_t count, loff_t *ppos)
{
	return monitor_common_write(filp, u_buf, count);
}

static ssize_t monitor_oob_read(struct file *filp,
				char __user *u_buf, size_t count)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);
	s32 val;
	int ret;


	switch (mon->protocol) {
	case EVL_EVENT_MASK:
		/* Disjunctive operation. */
	        ret = wait_mask_oob(mon, filp->f_flags, NULL, -1, false, &val);
		if (ret)
			return ret;
		break;
	case EVL_EVENT_COUNT:
		val = 1;
		ret = wait_count_oob(mon, filp->f_flags, NULL);
		break;
	default:
		ret = -EINVAL;
	}

	return ret ?: copy_to_user(u_buf, &val, sizeof(val)) ? -EFAULT : sizeof(val);
}

static ssize_t monitor_oob_write(struct file *filp,
				const char __user *u_buf, size_t count)
{
	return monitor_common_write(filp, u_buf, count);
}

static __poll_t monitor_poll(struct file *filp, poll_table *wait)
{
	struct evl_monitor *mon = element_of(filp, struct evl_monitor);
	__poll_t ret = 0;
	int val;

	if (mon->type != EVL_MONITOR_EVENT || mon->protocol != EVL_EVENT_MASK)
		return -EINVAL;

	poll_wait(filp, &mon->inband_wait_r, wait);
	poll_wait(filp, &mon->inband_wait_w, wait);

	val = atomic_read(__ATOMIC32(&mon->sstate->u.event.value));
	if (val)
		ret |= POLLIN|POLLRDNORM;
	else
		ret |= POLLOUT|POLLWRNORM;

	return ret;
}

static const struct file_operations monitor_fops = {
	.open		= evl_open_element,
	.release	= monitor_release,
	.read		= monitor_read,
	.write		= monitor_write,
	.poll		= monitor_poll,
	.unlocked_ioctl	= monitor_ioctl,
	.oob_read	= monitor_oob_read,
	.oob_write	= monitor_oob_write,
	.oob_ioctl	= monitor_oob_ioctl,
	.oob_poll	= monitor_oob_poll,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_ptr_ioctl,
	.compat_oob_ioctl = compat_ptr_oob_ioctl,
#endif
};

long evl_functl_monitor(struct evl_monitor *mon,
		unsigned int cmd, unsigned long arg)
{
	if (running_inband())
		return __monitor_ioctl(mon, 0, cmd,
				(unsigned long)compat_ptr(arg));

	return __monitor_oob_ioctl(mon, 0, cmd,
				(unsigned long)compat_ptr(arg));
}

static struct evl_element *
monitor_factory_build(struct evl_factory *fac, const char __user *u_name,
		void __user *u_attrs, int clone_flags, u32 *sstate_offp)
{
	struct __evl_monitor_sstate *sstate;
	struct evl_monitor_attrs attrs;
	struct evl_monitor *mon;
	struct evl_clock *clock;
	int ret;

	if (clone_flags & ~EVL_MONITOR_CLONE_FLAGS)
		return ERR_PTR(-EINVAL);

	ret = copy_from_user(&attrs, u_attrs, sizeof(attrs));
	if (ret)
		return ERR_PTR(-EFAULT);

	switch (attrs.type) {
	case EVL_MONITOR_GATE:
		switch (attrs.protocol) {
		case EVL_GATE_PP:
			if (attrs.ceiling < 1 ||
				attrs.ceiling > EVL_FIFO_MAX_PRIO)
				return ERR_PTR(-EINVAL);
			break;
		case EVL_GATE_PI:
			if (attrs.ceiling)
				return ERR_PTR(-EINVAL);
			break;
		default:
			return ERR_PTR(-EINVAL);
		}
		break;
	case EVL_MONITOR_EVENT:
		switch (attrs.protocol) {
		case EVL_EVENT_GATED:
		case EVL_EVENT_COUNT:
		case EVL_EVENT_MASK:
			break;
		default:
			return ERR_PTR(-EINVAL);
		}
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	clock = evl_get_clock_by_fd(attrs.clockfd);
	if (clock == NULL)
		return ERR_PTR(-EINVAL);

	mon = kzalloc(sizeof(*mon), GFP_KERNEL);
	if (mon == NULL) {
		ret = -ENOMEM;
		goto fail_alloc;
	}

	ret = evl_init_user_element(&mon->element, &evl_monitor_factory,
				u_name, clone_flags);
	if (ret)
		goto fail_element;

	sstate = evl_zalloc_chunk(&evl_shared_heap, sizeof(*sstate));
	if (sstate == NULL) {
		ret = -ENOMEM;
		goto fail_heap;
	}

	switch (attrs.type) {
	case EVL_MONITOR_GATE:
		sstate->u.gate.recursive = attrs.recursive;
		switch (attrs.protocol) {
		case EVL_GATE_PP:
			sstate->u.gate.ceiling = attrs.ceiling;
			evl_init_mutex_pp(&mon->mutex, clock,
					__ATOMIC32(&sstate->u.gate.owner),
					/* CAUTION: we do want the ceiling addr in sstate. */
					&sstate->u.gate.ceiling);
			break;
		case EVL_GATE_PI:
			evl_init_mutex_pi(&mon->mutex, clock,
					__ATOMIC32(&sstate->u.gate.owner));
			break;
		}
		raw_spin_lock_init(&mon->lock);
		INIT_LIST_HEAD(&mon->events);
		break;
	case EVL_MONITOR_EVENT:
		evl_init_wait(&mon->wait_queue, clock, EVL_WAIT_PRIO);
		sstate->u.event.gate_offset = EVL_MONITOR_NOGATE;
		atomic_set(__ATOMIC32(&sstate->u.event.value), attrs.initval);
		evl_init_poll_head(&mon->poll_head);
		init_waitqueue_head(&mon->inband_wait_r);
		init_waitqueue_head(&mon->inband_wait_w);
		init_irq_work(&mon->inband_wake_r, inband_wake_r_irqwork);
		init_irq_work(&mon->inband_wake_w, inband_wake_w_irqwork);
	}

	/*
	 * The type information is critical for the kernel sanity,
	 * don't allow userland to mess with it, so don't trust the
	 * shared state for this.
	 */
	mon->type = attrs.type;
	mon->protocol = attrs.protocol;
	mon->sstate = sstate;
	*sstate_offp = evl_shared_offset(sstate);
	sstate->shdr.fundle = evl_add_ns(&evl_core_ns, &mon->element, monitor);
	/*
	 * CAUTION: the following is only a courtesy to userland. We
	 * never, ever rely on this information which might be
	 * corrupted/tampered with by a broken/rogue user.
	 */
	sstate->type = mon->type;
	sstate->protocol = mon->protocol;

	return &mon->element;

fail_heap:
	evl_destroy_element(&mon->element);
fail_element:
	kfree(mon);
fail_alloc:
	evl_put_clock(clock);

	return ERR_PTR(ret);
}

static void monitor_factory_dispose(struct evl_element *e)
{
	struct evl_monitor *mon;
	unsigned long flags;

	mon = container_of(e, struct evl_monitor, element);

	evl_remove_ns(&evl_core_ns, e, monitor);

	if (mon->type == EVL_MONITOR_EVENT) {
		evl_put_clock(mon->wait_queue.clock);
		evl_destroy_wait(&mon->wait_queue);
		if (mon->gate) {
			raw_spin_lock_irqsave(&mon->gate->lock, flags);
			list_del(&mon->next);
			raw_spin_unlock_irqrestore(&mon->gate->lock, flags);
		}
	} else {
		evl_put_clock(mon->mutex.clock);
		evl_destroy_mutex(&mon->mutex);
	}

	evl_free_chunk(&evl_shared_heap, mon->sstate);
	evl_destroy_element(&mon->element);
	kfree_rcu(mon, element.rcu);
}

static ssize_t state_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct __evl_monitor_sstate *sstate;
	struct evl_thread *owner = NULL;
	struct evl_monitor *mon;
	ssize_t ret = 0;
	fundle_t fun;

	mon = evl_get_element_by_dev(dev, struct evl_monitor);
	if (mon == NULL)
		return -EIO;

	sstate = mon->sstate;

	if (mon->type == EVL_MONITOR_EVENT) {
		switch (mon->protocol) {
		case EVL_EVENT_MASK:
			ret = snprintf(buf, PAGE_SIZE, "%#x\n",
			       atomic_read(__ATOMIC32(&sstate->u.event.value)));
			break;
		case EVL_EVENT_COUNT:
			ret = snprintf(buf, PAGE_SIZE, "%d\n",
			       atomic_read(__ATOMIC32(&sstate->u.event.value)));
			break;
		case EVL_EVENT_GATED:
			ret = snprintf(buf, PAGE_SIZE, "%#x\n",	sstate->flags.all);
			break;
		}
	} else {
		fun = atomic_read(__ATOMIC32(&sstate->u.gate.owner));
		if (fun != EVL_NO_HANDLE) {
			owner = evl_lookup_ns_any(&evl_core_ns,	fun, thread);
			if (!owner)
				goto no_owner;
			ret = snprintf(buf, PAGE_SIZE, "%s(%d) %u %u\n",
				evl_element_name(&owner->element),
				evl_get_inband_pid(owner),
				sstate->u.gate.ceiling,
				sstate->u.gate.recursive ? sstate->u.gate.nesting : 1);
			evl_put_element(&owner->element);
		} else {
		no_owner:
			ret = snprintf(buf, PAGE_SIZE, "-1 %u 0\n",
				sstate->u.gate.ceiling);
		}
	}

	evl_put_element(&mon->element);

	return ret;
}
static DEVICE_ATTR_RO(state);

static struct attribute *monitor_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(monitor);

struct evl_factory evl_monitor_factory = {
	.name	=	EVL_MONITOR_DEV,
	.fops	=	&monitor_fops,
	.build =	monitor_factory_build,
	.dispose =	monitor_factory_dispose,
	.nrdev	=	CONFIG_EVL_NR_MONITORS,
	.attrs	=	monitor_groups,
	.flags	=	EVL_FACTORY_CLONE,
};

