/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>.
 */
#ifndef _LINUX_INBAND_WORK_H
#define _LINUX_INBAND_WORK_H

#ifdef CONFIG_IRQ_PIPELINE

#include <linux/irq_work.h>
#include <linux/workqueue.h>

struct inband_work_struct {
	struct irq_work irq_work;
	struct work_struct work;
	int cancel_pending;	/* xchg(int) is supported on all archs. */
};

void inband_work_trigger(struct irq_work *irq_work);

#define INIT_INBAND_WORK(__iwork, __func)				\
	do {								\
		INIT_WORK(&(__iwork)->work, __func);			\
		init_irq_work(&(__iwork)->irq_work, inband_work_trigger); \
		(__iwork)->cancel_pending = false;			\
	} while (0)

static inline bool schedule_inband_work(struct inband_work_struct *iwork)
{
	if (unlikely(running_inband()))
		return schedule_work(&iwork->work);

	if (!xchg(&iwork->cancel_pending, false))
		return irq_work_queue(&iwork->irq_work);

	return true;
}

static inline
bool schedule_inband_work_on(int cpu, struct inband_work_struct *iwork)
{
	if (unlikely(running_inband()))
		return schedule_work_on(cpu, &iwork->work);

	if (!xchg(&iwork->cancel_pending, false))
		return irq_work_queue_on(&iwork->irq_work, cpu);

	return true;
}

static inline
bool cancel_inband_work(struct inband_work_struct *iwork)
{
	if (unlikely(running_inband()))
		return cancel_work(&iwork->work);

	if (xchg(&iwork->cancel_pending, true))
		return true;

	return irq_work_queue(&iwork->irq_work);
}

static inline
bool cancel_inband_work_sync(struct inband_work_struct *iwork)
{
	if (running_inband())
		return cancel_work_sync(&iwork->work);

	/*
	 * We can't yield to in-band, so complain loudly for heads up
	 * then make best-effort.
	 */
	WARN_ON_ONCE(1);

	return cancel_inband_work(iwork);
}

static inline
bool flush_inband_work(struct inband_work_struct *iwork)
{
	if (running_inband())
		return flush_work(&iwork->work);

	/* See cancel_inband_work_sync(). */
	WARN_ON_ONCE(1);

	return false;
}

/* NOTE: the batch ring should always have at least one free entry. */
#define __INBAND_BATCH_WORK(__tag, __handler, __count)			\
	static inline void __PASTE(__tag, __trampoline)(struct irq_work *irq_work) \
	{								\
		struct __tag *__r = container_of(irq_work, struct __tag, irq_work); \
		schedule_work(&__r->work);				\
	}								\
	static inline void __PASTE(__tag, __batch)(struct work_struct *work) \
	{								\
		struct __tag *__r = container_of(work, struct __tag, work); \
		while (__r->tail != READ_ONCE(__r->head)) {		\
			int tail = __r->tail;				\
			__typeof(&__r->data[0]) __d = __r->data + tail; \
			__handler(__d);					\
			WRITE_ONCE(__r->tail, (tail + 1) % (__count));	\
		}							\
	}								\
	static inline void __PASTE(queue_, __tag)(struct __tag *__r, __typeof((&(__r)->data[0])) d) \
	{								\
		int head = __r->head;					\
		__typeof(d) _d = __r->data + head;			\
		*_d = *d;						\
		WRITE_ONCE(__r->head, (head + 1) % __count);		\
		irq_work_queue(&__r->irq_work);				\
		WARN_ON_ONCE(__r->head == READ_ONCE(__r->tail));	\
	}

#define INBAND_BATCH_WORK_INITIALIZER(__tag, __name)			\
	(struct __tag){							\
		.irq_work = IRQ_WORK_INIT(__PASTE(__tag, __trampoline)), \
		.work = __WORK_INITIALIZER((__name).work, __PASTE(__tag, __batch)), \
		.head = 0,						\
		.tail = 0,						\
	}

#define INBAND_BATCH_WORK(__tag, __handler, __data, __count)		\
	struct __tag {							\
		__typeof(__data) data[__count];				\
		struct irq_work irq_work;				\
		struct work_struct work;				\
		int head;						\
		int tail;						\
	};								\
	__INBAND_BATCH_WORK(__tag, __handler, __count);			\
	static inline void __PASTE(init_, __tag)(struct __tag *r)	\
	{								\
		init_irq_work(&r->irq_work, __PASTE(__tag, __trampoline)); \
		INIT_WORK(&r->work, __PASTE(__tag, __batch));		\
		r->head = 0;						\
		r->tail = 0;						\
	}								\
	static inline void __PASTE(destroy_, __tag)(struct __tag *r)	\
	{								\
	}

#define DECLARE_INBAND_BATCH_WORK(__tag, __name)			\
	struct __tag __name

#define DEFINE_INBAND_BATCH_WORK(__tag, __name)				\
	struct __tag __name = INBAND_BATCH_WORK_INITIALIZER(__tag, __name)

#define INBAND_BATCH_WORK_DYNAMIC(__tag, __handler, __data)		\
	struct __tag {							\
		__typeof(__data) *data;					\
		struct irq_work irq_work;				\
		struct work_struct work;				\
		int head;						\
		int tail;						\
		int count;						\
	};								\
	__INBAND_BATCH_WORK(__tag, __handler, __r->count);		\
	static inline int __PASTE(init_, __tag)(struct __tag *r, int count, gfp_t gfp) \
	{								\
		init_irq_work(&r->irq_work, __PASTE(__tag, __trampoline)); \
		INIT_WORK(&r->work, __PASTE(__tag, __batch));		\
		r->head = 0;						\
		r->tail = 0;						\
		r->count = count;					\
		r->data = kmalloc(sizeof(r->data[0]) * count, gfp);	\
		return r->data ? 0 : -ENOMEM;				\
	}								\
	static inline void __PASTE(destroy_, __tag)(struct __tag *r)	\
	{								\
		kfree(r->data);						\
	}

#else  /* !CONFIG_IRQ_PIPELINE */

struct inband_work_struct {
	struct work_struct work;
};

#define INIT_INBAND_WORK(__iwork, __func)	INIT_WORK(&(__iwork)->work, __func)

static inline bool schedule_inband_work(struct inband_work_struct *iwork)
{
	return schedule_work(&iwork->work);
}

static inline
bool schedule_inband_work_on(int cpu, struct inband_work_struct *iwork)
{
	return schedule_work_on(cpu, &iwork->work);
}

static inline
bool cancel_inband_work(struct inband_work_struct *iwork)
{
	return cancel_work(&iwork->work);
}

static inline
bool cancel_inband_work_sync(struct inband_work_struct *iwork)
{
	return cancel_work_sync(&iwork->work);
}

static inline
bool flush_inband_work(struct inband_work_struct *iwork)
{
	return flush_work(&iwork->work);
}

#endif /* !CONFIG_IRQ_PIPELINE */

#endif /* _LINUX_INBAND_WORK_H */
