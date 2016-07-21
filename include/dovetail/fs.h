/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DOVETAIL_FS_H
#define _DOVETAIL_FS_H

#ifdef CONFIG_DOVETAIL

#include <linux/irq_work.h>

struct oob_file_state {
	/* a pointer to some extended context. */
	void *data;
	/* for fput() deferral from oob to in-band. */
	struct irq_work irq_work;
};

#define __schedule_inband_fput(__irq_work)	\
	schedule_delayed_fput(container_of(irq_work, struct file, f_oob_state.irq_work))

static inline void schedule_inband_fput(struct oob_file_state *state,
					void (*handler)(struct irq_work *irq_work))
{
	init_irq_work(&state->irq_work, handler);
	irq_work_queue(&state->irq_work);
}

static inline void init_oob_fstate(struct oob_file_state *state)
{
	state->data = NULL;
}

#else  /* !CONFIG_DOVETAIL */

struct oob_file_state {
};

static inline void init_oob_fstate(struct oob_file_state *state)
{ }

struct irq_work;

static inline void __schedule_inband_fput(struct irq_work *irq_work)
{ }

static inline void schedule_inband_fput(struct oob_file_state *state,
					void (*handler)(struct irq_work *irq_work))

{ }

#endif	/* !CONFIG_DOVETAIL */

#endif /* !_DOVETAIL_FS_H */
