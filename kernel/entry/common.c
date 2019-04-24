// SPDX-License-Identifier: GPL-2.0

#include <linux/irq-entry-common.h>
#include <linux/irq_pipeline.h>
#include <linux/resume_user_mode.h>
#include <linux/highmem.h>
#include <linux/jump_label.h>
#include <linux/kmsan.h>
#include <linux/livepatch.h>
#include <linux/tick.h>

/* Workaround to allow gradual conversion of architecture code */
void __weak arch_do_signal_or_restart(struct pt_regs *regs) { }

#ifdef CONFIG_HAVE_GENERIC_TIF_BITS
#define EXIT_TO_USER_MODE_WORK_LOOP	(EXIT_TO_USER_MODE_WORK & ~_TIF_RSEQ)
#else
#define EXIT_TO_USER_MODE_WORK_LOOP	(EXIT_TO_USER_MODE_WORK)
#endif

/* TIF bits, which prevent a time slice extension. */
#ifdef CONFIG_PREEMPT_RT
/*
 * Since rseq slice ext has a direct correlation to the worst case
 * scheduling latency (schedule is delayed after all), only have it affect
 * LAZY reschedules on PREEMPT_RT for now.
 *
 * However, since this delay is only applicable to userspace, a value
 * for rseq_slice_extension_nsec that is strictly less than the worst case
 * kernel space preempt_disable() region, should mean the scheduling latency
 * is not affected, even for !LAZY.
 *
 * However, since this value depends on the hardware at hand, it cannot be
 * pre-determined in any sensible way. Hence punt on this problem for now.
 */
# define TIF_SLICE_EXT_SCHED	(_TIF_NEED_RESCHED_LAZY)
#else
# define TIF_SLICE_EXT_SCHED	(_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY)
#endif
#define TIF_SLICE_EXT_DENY	(EXIT_TO_USER_MODE_WORK & ~TIF_SLICE_EXT_SCHED)

static __always_inline unsigned long __exit_to_user_mode_loop(struct pt_regs *regs,
							      unsigned long ti_work)
{
	/*
	 * Before returning to user space ensure that all pending work
	 * items have been completed.
	 */
	while (ti_work & EXIT_TO_USER_MODE_WORK_LOOP) {

		local_irq_enable_exit_to_user(ti_work);

		/*
		 * Check that local_irq_enable_exit_to_user() does the
		 * right thing when pipelining.
		 */
		WARN_ON_ONCE(irq_pipeline_debug() && hard_irqs_disabled());

		if (ti_work & (_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY)) {
			if (!rseq_grant_slice_extension(ti_work & TIF_SLICE_EXT_DENY))
				schedule();
		}

		if (ti_work & _TIF_UPROBE)
			uprobe_notify_resume(regs);

		if (ti_work & _TIF_PATCH_PENDING)
			klp_update_patch_state(current);

		if (ti_work & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL))
			arch_do_signal_or_restart(regs);

		if (ti_work & _TIF_NOTIFY_RESUME)
			resume_user_mode_work(regs);

		/* Architecture specific TIF work */
		arch_exit_to_user_mode_work(regs, ti_work);

		/*
		 * Disable interrupts and reevaluate the work flags as they
		 * might have changed while interrupts and preemption was
		 * enabled above.
		 */
		local_irq_disable_exit_to_user();

		/* Check if any of the above work has queued a deferred wakeup */
		tick_nohz_user_enter_prepare();

		WARN_ON_ONCE(irq_pipeline_debug() && !hard_irqs_disabled());
		ti_work = read_thread_flags();
	}

	/* Return the latest work state for arch_exit_to_user_mode() */
	return ti_work;
}

/**
 * exit_to_user_mode_loop - do any pending work before leaving to user space
 * @regs:	Pointer to pt_regs on entry stack
 * @ti_work:	TIF work flags as read by the caller
 */
__always_inline unsigned long exit_to_user_mode_loop(struct pt_regs *regs,
						     unsigned long ti_work)
{
	for (;;) {
		ti_work = __exit_to_user_mode_loop(regs, ti_work);

		if (likely(!rseq_exit_to_user_mode_restart(regs, ti_work)))
			return ti_work;
		ti_work = read_thread_flags();
	}
}

noinstr irqentry_state_t irqentry_enter(struct pt_regs *regs)
{
	irqentry_state_t ret = {
		.exit_rcu = false,
#ifdef CONFIG_IRQ_PIPELINE
		.stage_info = IRQENTRY_INBAND_STALLED,
#endif
	};

#ifdef CONFIG_IRQ_PIPELINE
	if (running_oob()) {
		WARN_ON_ONCE(irq_pipeline_debug() && oob_irqs_disabled());
		ret.stage_info = IRQENTRY_OOB;
		return ret;
	}
#endif

	if (user_mode(regs)) {
#ifdef CONFIG_IRQ_PIPELINE
		ret.stage_info = IRQENTRY_INBAND_UNSTALLED;
#endif
		irqentry_enter_from_user_mode(regs);
		return ret;
	}

#ifdef CONFIG_IRQ_PIPELINE
	/*
	 * IRQ pipeline: If we trapped from kernel space, the virtual
	 * state may or may not match the hardware state. Since hard
	 * irqs are off on entry, we have to stall the in-band
	 * stage. Keep note of the unstalled state on entry through.
	 */
	if (!test_and_stall_inband_nocheck())
		ret.stage_info = IRQENTRY_INBAND_UNSTALLED;
#endif

	/*
	 * If this entry hit the idle task invoke ct_irq_enter() whether
	 * RCU is watching or not.
	 *
	 * Interrupts can nest when the first interrupt invokes softirq
	 * processing on return which enables interrupts.
	 *
	 * Scheduler ticks in the idle task can mark quiescent state and
	 * terminate a grace period, if and only if the timer interrupt is
	 * not nested into another interrupt.
	 *
	 * Checking for rcu_is_watching() here would prevent the nesting
	 * interrupt to invoke ct_irq_enter(). If that nested interrupt is
	 * the tick then rcu_flavor_sched_clock_irq() would wrongfully
	 * assume that it is the first interrupt and eventually claim
	 * quiescent state and end grace periods prematurely.
	 *
	 * Unconditionally invoke ct_irq_enter() so RCU state stays
	 * consistent.
	 *
	 * TINY_RCU does not support EQS, so let the compiler eliminate
	 * this part when enabled.
	 */
	if (!IS_ENABLED(CONFIG_TINY_RCU) &&
	    (is_idle_task(current) || arch_in_rcu_eqs())) {
		/*
		 * If RCU is not watching then the same careful
		 * sequence vs. lockdep and tracing is required
		 * as in irqentry_enter_from_user_mode().
		 */
		lockdep_hardirqs_off(CALLER_ADDR0);
		ct_irq_enter();
		instrumentation_begin();
		kmsan_unpoison_entry_regs(regs);
		trace_hardirqs_off_finish();
		instrumentation_end();

		ret.exit_rcu = true;
		return ret;
	}

	/*
	 * If RCU is watching then RCU only wants to check whether it needs
	 * to restart the tick in NOHZ mode. rcu_irq_enter_check_tick()
	 * already contains a warning when RCU is not watching, so no point
	 * in having another one here.
	 */
	lockdep_hardirqs_off(CALLER_ADDR0);
	instrumentation_begin();
	kmsan_unpoison_entry_regs(regs);
	rcu_irq_enter_check_tick();
	trace_hardirqs_off_finish();
	instrumentation_end();

	return ret;
}

/**
 * arch_irqentry_exit_need_resched - Architecture specific need resched function
 *
 * Invoked from raw_irqentry_exit_cond_resched() to check if resched is needed.
 * Defaults return true.
 *
 * The main purpose is to permit arch to avoid preemption of a task from an IRQ.
 */
static inline bool arch_irqentry_exit_need_resched(void);

#ifndef arch_irqentry_exit_need_resched
static inline bool arch_irqentry_exit_need_resched(void) { return true; }
#endif

void raw_irqentry_exit_cond_resched(void)
{
	if (!preempt_count()) {
		/* Sanity check RCU and thread stack */
		rcu_irq_exit_check_preempt();
		if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
			WARN_ON_ONCE(!on_thread_stack());
		if (need_resched() && arch_irqentry_exit_need_resched())
			preempt_schedule_irq();
	}
}
#ifdef CONFIG_PREEMPT_DYNAMIC
#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
DEFINE_STATIC_CALL(irqentry_exit_cond_resched, raw_irqentry_exit_cond_resched);
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
DEFINE_STATIC_KEY_TRUE(sk_dynamic_irqentry_exit_cond_resched);
void dynamic_irqentry_exit_cond_resched(void)
{
	if (!static_branch_unlikely(&sk_dynamic_irqentry_exit_cond_resched))
		return;
	raw_irqentry_exit_cond_resched();
}
#endif
#endif

#ifdef CONFIG_IRQ_PIPELINE

static inline
bool irqexit_may_preempt_schedule(irqentry_state_t state,
				struct pt_regs *regs)
{
	return state.stage_info == IRQENTRY_INBAND_UNSTALLED;
}

#else

static inline
bool irqexit_may_preempt_schedule(irqentry_state_t state,
				struct pt_regs *regs)
{
	return !regs_irqs_disabled(regs);
}

#endif

#ifdef CONFIG_IRQ_PIPELINE

static bool irqentry_syncstage(irqentry_state_t state) /* inband, hard irqs off */
{
	/*
	 * If pipelining interrupts, enable in-band IRQs then
	 * synchronize the interrupt log on exit if:
	 *
	 * - the inband stage was NOT stalled on kernel entry.
	 *
	 * - we entered the kernel over the oob stage, thus had to go
	 * through a stage migration (e.g. so that some inband handler
	 * could deal with a CPU exception).
	 *
	 * We run before preempt_schedule_irq() may be called later on
	 * by preemptible kernels, so that any rescheduling request
	 * triggered by in-band IRQ handlers is considered.
	 */
	if (state.stage_info == IRQENTRY_INBAND_UNSTALLED ||
		state.stage_info == IRQENTRY_OOB) {
		unstall_inband_nocheck();
		synchronize_pipeline_on_irq();
		stall_inband_nocheck();
		return true;
	}

	return false;
}

static void irqentry_unstall(void)
{
	unstall_inband_nocheck();
}

#else

static bool irqentry_syncstage(irqentry_state_t state)
{
	return false;
}

static void irqentry_unstall(void)
{
}

#endif

noinstr void irqentry_exit(struct pt_regs *regs, irqentry_state_t state)
{
	bool synchronized;

	if (running_oob())
		return;

	lockdep_assert_irqs_disabled();

	/* Check whether this returns to user mode */
	if (user_mode(regs)) {
		irqentry_exit_to_user_mode(regs);
		return;
	}

	instrumentation_begin();
	synchronized = irqentry_syncstage(state);

	if (irqexit_may_preempt_schedule(state, regs)) {
		/*
		 * If RCU was not watching on entry this needs to be done
		 * carefully and needs the same ordering of lockdep/tracing
		 * and RCU as the return to user mode path.
		 */
		if (state.exit_rcu) {
			/* Tell the tracer that IRET will enable interrupts */
			trace_hardirqs_on_prepare();
			lockdep_hardirqs_on_prepare();
			instrumentation_end();
			ct_irq_exit();
			lockdep_hardirqs_on(CALLER_ADDR0);
			goto out;
		}

		if (IS_ENABLED(CONFIG_PREEMPTION))
			irqentry_exit_cond_resched();

		/* Covers both tracing and lockdep */
		trace_hardirqs_on();
		instrumentation_end();
	} else {
		instrumentation_end();
		/*
		 * IRQ flags state is correct already. Just tell RCU if it
		 * was not watching on entry.
		 */
		if (state.exit_rcu)
			ct_irq_exit();
	}
out:
	if (synchronized)
		irqentry_unstall();
}

irqentry_state_t noinstr irqentry_nmi_enter(struct pt_regs *regs)
{
	irqentry_state_t irq_state;

	irq_state.lockdep = lockdep_hardirqs_enabled();

	__nmi_enter();
	lockdep_hardirqs_off(CALLER_ADDR0);
	lockdep_hardirq_enter();
	ct_nmi_enter();

	instrumentation_begin();
	kmsan_unpoison_entry_regs(regs);
	trace_hardirqs_off_finish();
	ftrace_nmi_enter();
	instrumentation_end();

	return irq_state;
}

void noinstr irqentry_nmi_exit(struct pt_regs *regs, irqentry_state_t irq_state)
{
	instrumentation_begin();
	ftrace_nmi_exit();
	if (irq_state.lockdep) {
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare();
	}
	instrumentation_end();

	ct_nmi_exit();
	lockdep_hardirq_exit();
	if (irq_state.lockdep)
		lockdep_hardirqs_on(CALLER_ADDR0);
	__nmi_exit();
}
