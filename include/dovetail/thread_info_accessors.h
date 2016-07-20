/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DOVETAIL_THREAD_INFO_ACCESSORS_H
#define _DOVETAIL_THREAD_INFO_ACCESSORS_H

#include <linux/types.h>
#include <linux/thread_info.h>

#define PIPELINE_MASK	BIT(0)
#define STAGE_MASK	BIT(1)

#ifdef CONFIG_IRQ_PIPELINE

static __always_inline void pipeline_enter(void)
{
	struct thread_info *ti = current_thread_info();

	ti->irq_pipeline_state.flags |= PIPELINE_MASK;
}

static __always_inline void pipeline_leave(void)
{
	struct thread_info *ti = current_thread_info();

	ti->irq_pipeline_state.flags &= ~PIPELINE_MASK;
}

static __always_inline void pipeline_oob_stage_enter(void)
{
	struct thread_info *ti = current_thread_info();

	ti->irq_pipeline_state.flags |= STAGE_MASK;
}

static __always_inline void pipeline_oob_stage_leave(void)
{
	struct thread_info *ti = current_thread_info();

	ti->irq_pipeline_state.flags &= ~STAGE_MASK;
}

static __always_inline bool in_pipeline(void)
{
	struct thread_info *ti = current_thread_info();

	return ti->irq_pipeline_state.flags & PIPELINE_MASK;
}

static __always_inline bool running_inband(void)
{
	struct thread_info *ti = current_thread_info();

	return !(ti->irq_pipeline_state.flags & STAGE_MASK);
}

static __always_inline bool running_oob(void)
{
	return !running_inband();
}

/* The current IRQ stage level: 0=inband, 1=oob */
static __always_inline unsigned int stage_level(void)
{
	return running_inband() ? 0 : 1;
}

#else /* !CONFIG_IRQ_PIPELINE */

static __always_inline void pipeline_enter(void)
{
}

static __always_inline void pipeline_leave(void)
{
}

static __always_inline void pipeline_oob_stage_enter(void)
{
}

static __always_inline void pipeline_oob_stage_leave(void)
{
}

static __always_inline bool in_pipeline(void)
{
	return false;
}

static __always_inline bool running_inband(void)
{
	return true;
}

static __always_inline bool running_oob(void)
{
	return false;
}

static __always_inline unsigned int stage_level(void)
{
	return 0;
}

#endif /* !CONFIG_IRQ_PIPELINE */

#endif /* !_DOVETAIL_THREAD_INFO_ACCESSORS_H */
