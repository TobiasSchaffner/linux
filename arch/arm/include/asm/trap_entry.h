/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASMARM_TRAP_ENTRY_H
#define _ASMARM_TRAP_ENTRY_H

#include <asm/dovetail.h>
#include <asm/trace/exceptions.h>

#ifdef CONFIG_MMU
#ifdef CONFIG_IRQ_PIPELINE

#include <linux/irqstage.h>

/*
 * We need to synchronize the virtual interrupt state with the hard
 * interrupt state we received on entry, then turn hardirqs back on to
 * allow code which does not require strict serialization to be
 * preempted by an out-of-band activity.
 */
static inline unsigned long dovetail_fault_entry(int exception,
						 struct pt_regs *regs)
{
	unsigned long flags;

	trace_ARM_trap_entry(exception, regs);

	flags = hard_local_save_flags();

	/*
	 * The companion core must demote the current context to
	 * in-band stage if running oob on entry.
	 */
	mark_trap_entry(exception, regs);

	if (raw_irqs_disabled_flags(flags)) {
		stall_inband();
		trace_hardirqs_off();
	}

	hard_local_irq_enable();

	return flags;
}

static inline void dovetail_fault_exit(int exception, struct pt_regs *regs,
				       unsigned long flags)
{
	WARN_ON_ONCE(irq_pipeline_debug() && hard_irqs_disabled());

	/*
	 * We expect kentry_exit_pipelined() to clear the stall bit if
	 * kentry_enter_pipelined() observed it that way.
	 */
	mark_trap_exit(exception, regs);
	trace_ARM_trap_exit(exception, regs);
	hard_local_irq_restore(flags);
}

#else /* !CONFIG_IRQ_PIPELINE */

static inline unsigned long dovetail_fault_entry(int exception,
						 struct pt_regs *regs)
{
	(void)(exception);
	(void)(regs);
	return 0;
}

static inline void dovetail_fault_exit(int exception, struct pt_regs *regs,
				       unsigned long combo)
{
	(void)exception;
	(void)regs;
	(void)combo;
}
#endif /* !CONFIG_IRQ_PIPELINE */

#else /* CONFIG_MMU */
static inline unsigned long dovetail_fault_entry(int exception,
						 struct pt_regs *regs)
{
	(void)(exception);
	(void)(regs);
	return 0;
}

static inline void dovetail_fault_exit(int exception, struct pt_regs *regs,
				       unsigned long combo)
{
	(void)exception;
	(void)regs;
	(void)combo;
}
#endif /* !CONFIG_MMU */

#endif
