// SPDX-License-Identifier: GPL-2.0
/*
 * EVL OOB blocking edge-stat RV monitor (composed automaton).
 *
 * This composed monitor tracks the per-CPU composed state of
 * (hard-IRQ-off windows, the OOB scheduler core, the OOB clock-tick
 * handler, and OOB hardware-IRQ handlers) using a precomputed
 * transition table generated from the topology in
 *   tools/verification/models/evl.dot
 * plus the metadata sidecar
 *   tools/verification/rvevl/models/evl.meta.yaml
 * by tools/verification/rvevl/scripts/emit-monitor.py and shipped
 * under
 *   include/generated/rv/evl_table.h
 *
 * Per CPU we keep:
 *   - a u8 holding the current composed state index;
 *   - a u64 timestamp captured on the last state transition;
 *   - one struct rv_edge_stat per composed edge (count/max/sum/hist);
 *   - a coverage bitmap of size RV_EVL_COV_LONGS longs.
 *
 * Hot path (__rv_evl_event):
 *   - Look up next = transition[state][event].
 *   - If next < 0 the event is irrelevant in this state -- bail.
 *   - Account dwell = now - state_entered_at into edges[edge_id]
 *     and mark coverage. Update state and timestamp.
 *
 * Event sources:
 *   - hard-IRQ-off transitions: routed from include/linux/rv_irqoff.h
 *     via the rv_irqoff_key static branch.
 *   - OOB scheduler: tracepoint probes registered in attach() /
 *     detached in detach().
 *   - OOB clock-tick: tracepoint probe on evl_core_tick (phase 0/1).
 *   - OOB hardware IRQ: tracepoint probes on irq_handler_entry/exit,
 *     filtered to actions with IRQF_OOB.
 *
 * All hot-path helpers are notrace + no_instrument_function and run
 * with hard IRQs effectively disabled (enforced by their callers --
 * the irqoff hooks fire under hard_local_irq_save, the EVL
 * tracepoints fire inside hard-IRQs-off windows, and
 * irq_handler_entry/exit fire from the IRQ entry path which is also
 * hard-IRQs-off).
 */

#include <linux/cache.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/rcupdate.h>
#include <linux/rv.h>
#include <linux/rv_edge_stat.h>
#include <linux/rv_irqoff.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/tracefs.h>
#include <linux/tracepoint.h>

#include <generated/rv/evl_table.h>

#include <trace/events/evl.h>
#include <trace/events/irq.h>

#define NR_EDGES	RV_EVL_NR_EDGES
#define COV_LONGS	RV_EVL_COV_LONGS

/*
 * Per-IRQ side-table for hwirq handler dwell.  Independent of the
 * state machine: tracks the [hwirq_enter, hwirq_exit] window keyed
 * by IRQ number, regardless of which IRQOFF_* state was preempted.
 * Bounded by RV_EVL_HWIRQ_MAX (covers all SiFive PLIC + IPIs/timer
 * on VisionFive2; IRQs >= cap are folded into bucket 0).
 */
#define RV_EVL_HWIRQ_MAX	256

struct rv_hwirq_stat {
	u64	count;
	u64	sum_ns;
	u64	max_ns;
};

struct rv_evl_pcpu {
	u8			state;
	bool			irqoff_shadow;
	bool			irqoff_idle;
	/*
	 * hwirq_depth: nesting level of irq_handler_entry/exit on this
	 * CPU. The FSM models only the OUTERMOST handler; inner levels
	 * have their FSM event suppressed -- otherwise a nested
	 * hwirq_enter from HWIRQ_OVER_IRQOFF_* would either land on a
	 * state with no matching hwirq_enter edge (the in-handler
	 * HWIRQ_NESTED route only fans in from HWIRQ_OVER_RUNNING_IRQS_ON,
	 * see model rationale), or consume the OUTER hwirq_exit_over_*
	 * edge prematurely and strand subsequent events. Per-IRQ dwell
	 * accounting (cur_hwirq*) is also gated by depth==0 so a nested
	 * IRQ does not attribute the outer's start time to its own bucket.
	 *
	 * hwirq_outer_suppressed: the outermost hwirq_enter was dropped
	 * because state was OOB_SCHED_SWITCHING (an opaque window per the
	 * model rationale). The matching outermost hwirq_exit must be
	 * dropped too, otherwise the FSM sees an orphan exit in a state
	 * with no hwirq_exit edge (post-switch is normally IRQOFF_OOB or
	 * RUNNING_IRQS_ON).
	 */
	u8			hwirq_depth;
	bool			hwirq_outer_suppressed;
	u16			cur_hwirq;
	u64			state_entered_at;
	u64			cur_hwirq_start;
	/*
	 * Trap-arrival timestamp recorded by rv_irqoff_trap_enter() on
	 * every trap (regardless of SR_PIE), in the monotonic timebase
	 * (rv_evl_now == ktime_get_mono_fast_ns) -- the same base latmus
	 * uses for its timer deadline. Lets probe_irq_handler_entry()
	 * decompose hwirq_enter_over_irqoff_inband dwell into
	 *   blocker  = trap_kts - state_entered_at  (inband held off the trap)
	 *   dispatch = now      - trap_kts          (trap asm + glue to ISR)
	 * and is exposed via rv_evl_trap_arrival_mono() so latmus can pin
	 * the exact deadline->first-trap delta per wake.
	 */
	u64			trap_kts;
	/*
	 * Last inband-irqoff blocker on the wake path: trap_kts -
	 * state_entered_at when the timer trap landed in IRQOFF_INBAND
	 * (hard IRQs were held off), else 0. Read via
	 * rv_evl_trap_blocker_ns() so latmus can classify each head as
	 * hardware (blocker~0) vs software (blocker>0) per wake.
	 */
	u64			blocker_ns;
	u64			unknown;
	/*
	 * Longest inband-irqoff blocker seen: dwell + caller IP captured
	 * when a new max is recorded on hwirq_enter_over_irqoff_inband or
	 * irqoff_exit_inband edges (the two ways an inband hard-off window
	 * can block an EVL wake). Exposed via tracefs longest_irqoff.
	 */
	u64			longest_irqoff_ns;
	unsigned long		longest_irqoff_caller;
	unsigned long		irqoff_caller;  /* current window, captured in __rv_irqoff_off */
	struct rv_edge_stat	edges[NR_EDGES];
	unsigned long		coverage[COV_LONGS];
	struct rv_hwirq_stat	hwirq[RV_EVL_HWIRQ_MAX];
};

static DEFINE_PER_CPU_ALIGNED(struct rv_evl_pcpu, rv_evl_pcpu);

DEFINE_STATIC_KEY_FALSE(rv_irqoff_key);
EXPORT_SYMBOL_GPL(rv_irqoff_key);

static DEFINE_STATIC_KEY_FALSE(rv_evl_enabled_key);

/* Forward-declared so rv_evl_event() can pass its address to rv_react();
 * the storage and initializer are defined at the bottom of this file
 * (the registration struct fans out into half the hot-path callbacks).
 */
static struct rv_monitor rv_evl_monitor;

/*
 * Rate-limit the reactor call on unmodelled (state, event) pairs.
 * One burst of 10 lines per 5 s on each CPU is enough to spot
 * bring-up bugs without DoS'ing dmesg under sustained misbehaviour.
 */
static DEFINE_RATELIMIT_STATE(rv_evl_unknown_rs, 5 * HZ, 10);

/*
 * Hot-path event entry. Inline-friendly. Caller must guarantee that
 * hard IRQs are disabled, which is the case for every event source we
 * hook (see file header). state and state_entered_at are therefore
 * race-free per-CPU stores.
 */

/*
 * All FSM timestamps use the NMI-safe monotonic clock. Unlike
 * sched_clock(), ktime_get_mono_fast_ns() is guaranteed non-decreasing
 * across CPUs and contexts, so a per-edge dwell delta can never glitch
 * into a bogus multi-millisecond max (the JH7110 has no stable
 * cross-CPU sched_clock). It is also the exact base latmus uses for its
 * timer deadline, so trap-arrival stamps compare directly across both.
 */
static __always_inline notrace __attribute__((no_instrument_function))
u64 rv_evl_now(void)
{
	return ktime_get_mono_fast_ns();
}

/*
 * True while the FSM believes a hard-IRQ handler is running. Only these
 * three states have an outgoing hwirq_exit edge, and none of them has an
 * outgoing hwirq_enter edge -- so this predicate is the exact, model-
 * derived gate for the nesting guards in the hwirq probes below.
 */
static __always_inline notrace __attribute__((no_instrument_function))
bool rv_evl_in_hwirq_state(u8 state)
{
	return state == RV_EVL_S_IRQ_HANDLER_OVER_LINUX_IRQOFF ||
	       state == RV_EVL_S_IRQ_HANDLER_OVER_EVL_IRQOFF ||
	       state == RV_EVL_S_IRQ_HANDLER_SOFT_INJECTED ||
	       state == RV_EVL_S_IRQ_HANDLER_NESTED;
}

static __always_inline notrace __attribute__((no_instrument_function))
void rv_evl_event(unsigned int event)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);
	s16 next, edge_id;
	u64 now, dwell;

	/*
	 * Every caller passes a compile-time RV_EVL_EV_* constant; the
	 * generated header asserts the enum range, so no runtime bounds
	 * check on @event is necessary here.
	 */
	next = rv_evl_transition[p->state][event];
	if (next < 0) {
		/*
		 * Unmodelled (state, event) pair.  Count per CPU and,
		 * rate-limited, fire the reactor so the printk reactor
		 * shows one dmesg line per burst (vprintk_deferred()
		 * keeps it atomic / IRQ-off safe).
		 */
		p->unknown++;
		if (__ratelimit(&rv_evl_unknown_rs))
			rv_react(&rv_evl_monitor,
				 "rv: monitor rv_evl does not allow event %s on state %s\n",
				 rv_evl_event_labels[event],
				 rv_evl_state_labels[p->state]);
		return;
	}

	edge_id = rv_evl_edge_id[p->state][event];
	now = rv_evl_now();
	dwell = now - p->state_entered_at;
	/*
	 * Defensive only: rv_evl_now() (monotonic) is non-decreasing, but
	 * a probe-race during attach can still rebase state_entered_at
	 * after the now sample. Clamp negative deltas to 0 rather than
	 * wrapping into a ~2^64 ns bogus dwell that pollutes max_ns.
	 */
	if ((s64)dwell < 0)
		dwell = 0;

	rv_edge_stat_account(&p->edges[edge_id], dwell);
	rv_edge_coverage_mark(p->coverage, edge_id);

	/*
	 * Track longest inband-irqoff blocker: if this edge exits
	 * LINUX_TURNED_HARDIRQS_OFF and sets a new dwell max, save the
	 * caller IP captured when that window opened.
	 */
	if (p->state == RV_EVL_S_LINUX_TURNED_HARDIRQS_OFF &&
	    dwell > p->longest_irqoff_ns) {
		p->longest_irqoff_ns = dwell;
		p->longest_irqoff_caller = p->irqoff_caller;
	}

	/*
	 * The idle-WFI tag only applies as long as we stay in either
	 * IRQOFF_* state.  If anything else consumes the window
	 * (hwirq_enter, schedule) the next return to RUNNING_IRQS_ON
	 * is no longer pure idle sleep.
	 */
	if (p->state == RV_EVL_S_LINUX_TURNED_HARDIRQS_OFF ||
	    p->state == RV_EVL_S_EVL_TURNED_IRQS_OFF)
		p->irqoff_idle = false;

	p->state = (u8)next;
	p->state_entered_at = now;
}

/* ---- Hard-IRQ-off entry points (called from include/linux/rv_irqoff.h) ---- */

notrace __attribute__((no_instrument_function))
void __rv_irqoff_off(void)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	/*
	 * Per-CPU edge-detect. Two callers can race into this function
	 * with hard IRQs already off (e.g. the asm-generic pipeline
	 * macros and arch trap entry); only the first off->on->off
	 * transition should be reported to the state machine, otherwise
	 * the monitor sees spurious nested irqoff_enter events.
	 */
	if (p->irqoff_shadow)
		return;
	p->irqoff_shadow = true;
	p->irqoff_caller = (unsigned long)__builtin_return_address(0);
	/*
	 * Tag idle-task hard-IRQ-off windows. We still emit the
	 * IRQOFF_ENTER transition (otherwise a HWIRQ that wakes the
	 * CPU from WFI would fire from RUNNING_IRQS_ON and be flagged
	 * unmodelled), but we will suppress the resulting dwell on the
	 * matching irqoff_exit / scheduler-entry edge: WFI sleep is by
	 * construction not blocking and would otherwise dominate the
	 * histogram.
	 */
	p->irqoff_idle = is_idle_task(current);
	/*
	 * Inside the dovetail context-switch window (state
	 * OOB_SCHED_SWITCHING) we treat the CPU as a black box: hard
	 * IRQs may legitimately flip on briefly inside switch_to()
	 * before being re-disabled, and the next task may resume from
	 * a different point in its own scheduler chain.  Keep the
	 * shadow so the next real edge after switch_tail is detected
	 * correctly, but do not feed the automaton.
	 */
	if (p->state == RV_EVL_S_EVL_SCHEDULER_SWITCHING)
		return;
	/*
	 * Split the IRQOFF state by execution stage at entry time:
	 *   running_oob() == false  -> IRQOFF_INBAND (inband blocking OOB)
	 *   running_oob() == true   -> IRQOFF_OOB    (intra-OOB critical sec)
	 * Subsequent transitions out of either IRQOFF_* state share the
	 * same set of events (irqoff_exit, hwirq_enter, ...) but their
	 * dwell statistics are recorded separately, isolating the
	 * inband-blocking dwell from short OOB internal windows.
	 */
	if (running_oob())
		rv_evl_event(RV_EVL_EV_IRQOFF_ENTER_OOB);
	else
		rv_evl_event(RV_EVL_EV_IRQOFF_ENTER_INBAND);
}
EXPORT_SYMBOL_GPL(__rv_irqoff_off);

notrace __attribute__((no_instrument_function))
void __rv_irqoff_on(void)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	if (!p->irqoff_shadow)
		return;
	p->irqoff_shadow = false;
	if (p->state == RV_EVL_S_EVL_SCHEDULER_SWITCHING)
		return;
	/*
	 * If the off side was tagged as idle, rebase the dwell baseline
	 * to "now" so the irqoff_exit edge accounts ~0 ns instead of an
	 * idle-nap WFI sleep. WFI is never a blocker (a pending IRQ
	 * resumes it at once).
	 */
	if (p->irqoff_idle) {
		p->irqoff_idle = false;
		p->state_entered_at = rv_evl_now();
	}
	rv_evl_event(RV_EVL_EV_IRQOFF_EXIT);
}
EXPORT_SYMBOL_GPL(__rv_irqoff_on);

/* ---- OOB scheduler tracepoint probes ---- */

static void probe_evl_schedule(void *data, struct evl_rq *rq)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	/*
	 * __evl_schedule() runs from BOTH stages: the OOB common case
	 * and from inband during stage migration (evl_switch_oob ->
	 * dovetail_leave_inband -> inband_switch_tail -> resume_oob_task
	 * -> evl_schedule).  Only model the OOB invocations.
	 */
	if (!running_oob())
		return;
	/*
	 * If the previous switch_tail was never observed on this CPU
	 * (the usual reason being a fork-resumed OOB task whose first
	 * execution lands at ret_from_fork rather than inside
	 * finish_rq_switch), the automaton is still in
	 * OOB_SCHED_SWITCHING.  Synthesize the missing switch_tail so
	 * it leaves OOB_SCHED_SWITCHING before we record the new
	 * schedule entry.
	 *
	 * Rebase the dwell baseline to "now" first: the recorded
	 * state_entered_at belongs to a different thread's pick on this
	 * CPU (possibly seconds ago). Without this rebase the synthetic
	 * SWITCH_TAIL would inject an arbitrary cross-thread dwell into
	 * max_ns/sum_ns and the top histogram bucket.
	 */
	if (p->state == RV_EVL_S_EVL_SCHEDULER_SWITCHING) {
		p->state_entered_at = rv_evl_now();
		rv_evl_event(RV_EVL_EV_SWITCH_TAIL);
	}
	rv_evl_event(RV_EVL_EV_SCHEDULE);
}

static void probe_evl_pick_thread(void *data, struct evl_thread *next)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	if (!running_oob())
		return;
	if (p->state == RV_EVL_S_EVL_SCHEDULER_SWITCHING)
		return;
	/*
	 * Records the OOB_SCHED_PICKING -> OOB_SCHED_SWITCHING
	 * transition.  The remainder of __evl_schedule()
	 * (leave_inband / enter_inband / proxy-tick notification /
	 * dovetail_context_switch) may legitimately toggle hard IRQs
	 * and swap tasks; nothing on this CPU is meaningful to the
	 * automaton until switch_tail closes the window.  The
	 * black-box predicate is now simply
	 * `state == OOB_SCHED_SWITCHING`.
	 */
	rv_evl_event(RV_EVL_EV_PICK_THREAD);
}

static void probe_evl_switch_tail(void *data, struct evl_thread *curr)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	/*
	 * Close the black-box window.  If we never entered it on this
	 * CPU (e.g. fork-resumed task whose first appearance is here
	 * with no prior pick_thread on this CPU) treat as a no-op
	 * rather than firing an unmodelled transition.
	 */
	if (p->state != RV_EVL_S_EVL_SCHEDULER_SWITCHING)
		return;
	/*
	 * Split by post-switch stage.  dovetail_context_switch() may
	 * have left us on the inband stage ("inband_tail" path); in
	 * that case the next irqs-on event will be an inband one, so
	 * route the transition to IRQOFF_INBAND.  Otherwise we stayed
	 * OOB and continue from IRQOFF_OOB.  The two flavours have
	 * very different dwells (inband_tail also runs the Linux
	 * scheduler tail before releasing IRQs).
	 */
	if (running_oob())
		rv_evl_event(RV_EVL_EV_SWITCH_TAIL);
	else
		rv_evl_event(RV_EVL_EV_SWITCH_TAIL_INBAND);
}

/* ---- OOB hwirq tracepoint probes ---- */

/*
 * Called from arch trap-entry asm (rv_irqoff_trap_enter on RISC-V) on
 * EVERY trap arrival, gated only by the rv_irqoff_key static branch.
 * Zero overhead when the runtime switch is off.
 */
void notrace __attribute__((no_instrument_function))
__rv_evl_account_trap_arrival(void)
{
	__this_cpu_write(rv_evl_pcpu.trap_kts, rv_evl_now());
}
EXPORT_SYMBOL_GPL(__rv_evl_account_trap_arrival);

/*
 * Read the current CPU's last trap-arrival timestamp in the
 * CLOCK_MONOTONIC timebase. Returns 0 when the monitor's runtime switch
 * is off (nothing stamped), so callers such as the latmus driver can
 * detect "monitor not running" and skip the deadline->trap correlation.
 * A plain per-CPU read with no side effects.
 */
u64 rv_evl_trap_arrival_mono(void)
{
	if (!rv_irqoff_enabled())
		return 0;
	return this_cpu_read(rv_evl_pcpu.trap_kts);
}
EXPORT_SYMBOL_GPL(rv_evl_trap_arrival_mono);

/*
 * Read the current CPU's last wake-path inband-irqoff blocker (ns):
 * the time the timer trap waited because the inband stage held hard
 * IRQs off, or 0 if it arrived with IRQs enabled. Returns 0 when the
 * monitor's runtime switch is off. Pairs with rv_evl_trap_arrival_mono()
 * to split a wake's head into hardware vs software.
 */
u64 rv_evl_trap_blocker_ns(void)
{
	if (!rv_irqoff_enabled())
		return 0;
	return this_cpu_read(rv_evl_pcpu.blocker_ns);
}
EXPORT_SYMBOL_GPL(rv_evl_trap_blocker_ns);

static void probe_irq_handler_entry(void *data, int irq,
				    struct irqaction *action)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);
	u8 depth = p->hwirq_depth;

	/* Bound the depth counter; deeper nesting suppresses FSM as at depth=2. */
	if (depth < U8_MAX)
		p->hwirq_depth = depth + 1;

	/*
	 * Per-IRQ side-table is valid only for the OUTERMOST handler.
	 * Inner levels would overwrite cur_hwirq and wrongly attribute
	 * the outer dwell to the inner number.  Out-of-range IRQs are
	 * dropped (cur_hwirq_start=0) instead of aliased into bucket 0.
	 */
	if (depth == 0) {
		if (irq < 0 || irq >= RV_EVL_HWIRQ_MAX) {
			p->cur_hwirq_start = 0;
		} else {
			p->cur_hwirq = irq;
			p->cur_hwirq_start = rv_evl_now();
		}
	}

	/*
	 * Drive the FSM only on the outermost entry.  Nested IRQs do
	 * not change the composed state (the outer handler is still
	 * running over the same blocking window).
	 *
	 * Count every hwirq handler -- the action->flags IRQF_OOB bit
	 * is unreliable here: irq_switch_oob() promotes IRQs to OOB
	 * without setting it (e.g. the percpu riscv-timer registered
	 * with IRQF_TIMER only).  Every handler running with hard IRQs
	 * off extends the blocking window equally, regardless of which
	 * stage runs it.
	 *
	 * Also suppress inside OOB_SCHED_SWITCHING (opaque switch_to()
	 * window). Record the suppression so the matching outermost
	 * exit is dropped too.
	 */
	/*
	 * Nesting model. EVL re-enables hard IRQs on the *inband* handler
	 * entry (running_inband -> hard_cond_local_irq_enable), so a
	 * hardware IRQ can nest on top of the in-band wake handler -- and
	 * only there (OOB/soft handlers keep hard IRQs off). Drive the
	 * excursion from the depth counter:
	 *   depth 1->2 over IRQ_HANDLER_OVER_LINUX_IRQOFF : enter NESTED
	 *   deeper, or over an OOB/soft handler           : collapse
	 */
	if (depth == 1) {
		if (p->state == RV_EVL_S_IRQ_HANDLER_OVER_LINUX_IRQOFF)
			rv_evl_event(RV_EVL_EV_HWIRQ_ENTER); /* -> NESTED */
		return;
	}
	if (depth > 1)
		return;
	if (p->state == RV_EVL_S_EVL_SCHEDULER_SWITCHING) {
		p->hwirq_outer_suppressed = true;
		return;
	}
	/*
	 * Desync safety: if the FSM already runs a handler at depth 0
	 * (counter reset mid-handler), drop this enter so it cannot raise
	 * an unmodelled event.
	 *
	 * Use READ_ONCE for consistency with the exit-path orphan guard.
	 */
	u8 state = READ_ONCE(p->state);
	if (rv_evl_in_hwirq_state(state))
		return;
	p->hwirq_outer_suppressed = false;
	p->blocker_ns = 0;

	/*
	 * Edge-4 (hwirq_enter_over_irqoff_inband) split probe: when the
	 * upcoming rv_evl_event() will fire that edge, decompose its
	 * dwell into the part charged to the inband-irqoff blocker and
	 * the part charged to pure trap-dispatch glue. Emitted before
	 * the FSM transition so state_entered_at still reflects the
	 * IRQOFF_INBAND entry.
	 */
	if (p->state == RV_EVL_S_LINUX_TURNED_HARDIRQS_OFF && p->trap_kts) {
		u64 now = rv_evl_now();
		s64 dispatch = (s64)(now - p->trap_kts);
		s64 blocker  = (s64)(p->trap_kts - p->state_entered_at);
		if (dispatch < 0) dispatch = 0;
		if (blocker  < 0) blocker  = 0;
		p->blocker_ns = blocker;
		trace_evl_hwirq_split(irq, blocker, dispatch);
	}

	rv_evl_event(RV_EVL_EV_HWIRQ_ENTER);
}

static void probe_irq_handler_exit(void *data, int irq,
				   struct irqaction *action, int ret)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);
	struct rv_hwirq_stat *h;
	u64 dwell;
	u8 depth;

	if (p->hwirq_depth > 0)
		p->hwirq_depth--;
	depth = p->hwirq_depth;

	/*
	 * Attribute the dwell only on the outermost matched exit. The
	 * cur_hwirq* fields were set only on the outermost entry; a
	 * nested exit whose irq number happens to equal cur_hwirq would
	 * otherwise wrongly account the outer's start as the inner's
	 * dwell.  An irq-number mismatch indicates an interleaved or
	 * cascaded entry/exit and the sidecar is skipped.
	 */
	if (depth == 0 && p->cur_hwirq_start &&
	    irq >= 0 && irq < RV_EVL_HWIRQ_MAX &&
	    (u16)irq == p->cur_hwirq) {
		dwell = rv_evl_now() - p->cur_hwirq_start;
		/* Defensive clamp, as in rv_evl_event(). */
		if ((s64)dwell < 0)
			dwell = 0;
		h = &p->hwirq[p->cur_hwirq];
		h->count  += 1;
		h->sum_ns += dwell;
		if (dwell > h->max_ns)
			h->max_ns = dwell;
	}
	if (depth == 0)
		p->cur_hwirq_start = 0;

	/*
	 * Nesting model (mirror of the entry side):
	 *   depth 2->1 while in NESTED : pop back to the in-band handler
	 *   deeper pops                : collapse
	 *   depth 1->0                 : outermost exit
	 */
	if (depth == 1) {
		if (p->state == RV_EVL_S_IRQ_HANDLER_NESTED)
			rv_evl_event(RV_EVL_EV_HWIRQ_EXIT); /* NESTED -> OVER_LINUX */
		return;
	}
	if (depth > 1)
		return;
	/*
	 * If the outermost entry was suppressed (SWITCHING), drop this
	 * exit too; the FSM never saw the matching enter.
	 */
	if (p->hwirq_outer_suppressed) {
		p->hwirq_outer_suppressed = false;
		return;
	}
	/*
	 * State-based orphan guard: a hwirq_exit while the FSM is not in a
	 * handler state is an orphan (enter suppressed / counter desync);
	 * drop it instead of raising an unmodelled event.
	 *
	 * Use READ_ONCE to prevent compiler from reordering this check
	 * with the p->state read inside rv_evl_event (which is inlined).
	 * Without the barrier, the compiler might hoist the transition
	 * lookup above this guard, causing TOCTOU races.
	 */
	u8 state = READ_ONCE(p->state);
	if (!rv_evl_in_hwirq_state(state))
		return;
	rv_evl_event(RV_EVL_EV_HWIRQ_EXIT);
}

/* ---- attach / detach (rv_evl enable callbacks) ---- */

static void reset_ipi(void *unused);

static int rv_evl_attach(void)
{
	int ret;

	/*
	 * Clear per-CPU state BEFORE registering any probe.  Otherwise
	 * stale (cur_hwirq, cur_hwirq_start) left over from a previous
	 * enable session can be matched by the very first exit probe
	 * fired after re-enable, yielding a `now - stale_start`
	 * multi-second bogus dwell.
	 */
	on_each_cpu(reset_ipi, NULL, 1);

	/*
	 * Enable the IRQ-off routing key first so the state machine
	 * leaves RUNNING_IRQS_ON on the very next hard IRQ-off window. If we
	 * registered the tracepoint probes first, the first tick or
	 * hwirq on each CPU could fire before any irqoff_enter and
	 * trigger a spurious unmodelled transition from RUNNING_IRQS_ON. The
	 * irqoff hooks themselves are per-CPU edge-detected so a
	 * mid-window flip is self-correcting.
	 */
	static_branch_enable(&rv_irqoff_key);

	ret = register_trace_evl_schedule(probe_evl_schedule, NULL);
	if (ret)
		goto err_schedule;
	ret = register_trace_evl_pick_thread(probe_evl_pick_thread, NULL);
	if (ret)
		goto err_pick;
	ret = register_trace_evl_switch_tail(probe_evl_switch_tail, NULL);
	if (ret)
		goto err_switch_tail;
	/*
	 * Register the exit probe BEFORE the entry probe.  If we did
	 * the opposite, an IRQ entering in the window between the two
	 * registrations would arm cur_hwirq_start without ever being
	 * paired with an exit; the next exit for that same IRQ number
	 * (potentially seconds later) would then compute a bogus
	 * multi-second dwell.  With this order, an exit-without-entry
	 * is a harmless no-op (cur_hwirq_start == 0, ensured by the
	 * per-CPU reset above).
	 */
	ret = register_trace_irq_handler_exit(probe_irq_handler_exit, NULL);
	if (ret)
		goto err_irq_exit;
	ret = register_trace_irq_handler_entry(probe_irq_handler_entry, NULL);
	if (ret)
		goto err_irq_entry;

	return 0;

err_irq_entry:
	unregister_trace_irq_handler_exit(probe_irq_handler_exit, NULL);
err_irq_exit:
	unregister_trace_evl_switch_tail(probe_evl_switch_tail, NULL);
err_switch_tail:
	unregister_trace_evl_pick_thread(probe_evl_pick_thread, NULL);
err_pick:
	unregister_trace_evl_schedule(probe_evl_schedule, NULL);
err_schedule:
	static_branch_disable(&rv_irqoff_key);
	return ret;
}

static void rv_evl_detach(void)
{
	/* Stop the IRQ-off stream first; the others are independent. */
	static_branch_disable(&rv_irqoff_key);

	/* Mirror of attach: stop entries first, then exits. */
	unregister_trace_irq_handler_entry(probe_irq_handler_entry, NULL);
	unregister_trace_irq_handler_exit(probe_irq_handler_exit, NULL);
	unregister_trace_evl_switch_tail(probe_evl_switch_tail, NULL);
	unregister_trace_evl_pick_thread(probe_evl_pick_thread, NULL);
	unregister_trace_evl_schedule(probe_evl_schedule, NULL);

	/* Sync to ensure no probe is still running on any CPU. */
	tracepoint_synchronize_unregister();
}

/* ---- rv_evl glue ---- */

struct snap_args {
	unsigned int		edge;
	struct rv_edge_stat	*out;
};

static void snapshot_ipi(void *info)
{
	struct snap_args *a = info;
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	*a->out = p->edges[a->edge];
}

static void rv_evl_snapshot_edge(unsigned int cpu, unsigned int edge,
				      struct rv_edge_stat *out)
{
	struct snap_args a = { .edge = edge, .out = out };

	if (edge >= NR_EDGES)
		return;
	smp_call_function_single(cpu, snapshot_ipi, &a, 1);
}

static void cov_ipi(void *info)
{
	unsigned long *dst = info;
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);
	unsigned int i;

	for (i = 0; i < COV_LONGS; i++)
		dst[i] = p->coverage[i];
}

static void rv_evl_coverage_snapshot(unsigned int cpu, unsigned long *dst)
{
	smp_call_function_single(cpu, cov_ipi, dst, 1);
}

static void reset_ipi(void *unused)
{
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	memset(p->edges, 0, sizeof(p->edges));
	memset(p->coverage, 0, sizeof(p->coverage));
	memset(p->hwirq, 0, sizeof(p->hwirq));
	p->cur_hwirq_start = 0;
	p->hwirq_depth = 0;
	p->hwirq_outer_suppressed = false;
	p->unknown = 0;
	p->longest_irqoff_ns = 0;
	p->longest_irqoff_caller = 0;
	/*
	 * Resync the FSM to the initial (hard-IRQs-on) state and drop the
	 * edge-detect shadow. A reset runs from on_each_cpu() with hard
	 * IRQs off, and enabling the monitor patches static branches via
	 * stop_machine() -- a multi-ms hard-off window that must not be
	 * billed as an inband blocker once measurement starts. Discarding
	 * the state here is self-healing: a stale re-enable early-returns
	 * on the cleared shadow (no unmodelled irqoff_exit), a hwirq lands
	 * on the RUNNING->SOFT_INJECTED off-path edge, and the next real
	 * on->off edge re-arms the FSM cleanly.
	 */
	p->state = RV_EVL_INITIAL;
	p->irqoff_shadow = false;
	p->irqoff_idle = false;
	/*
	 * Rebase the dwell baseline to "now". Without this, the next
	 * edge would record a huge dwell measured from boot time (or
	 * from the previous reset window), polluting max_ns/sum_ns and
	 * the top histogram bucket forever.
	 */
	p->state_entered_at = rv_evl_now();
}

static void rv_evl_reset(void)
{
	/*
	 * Refuse to reset while the monitor is live: probes write to
	 * the per-CPU counters under hard-IRQs-off, the reset IPI
	 * memsets them; on 32-bit the u64s would tear, on 64-bit the
	 * histogram increments would race.  User-space must echo 0 to
	 * enable first.
	 */
	if (static_branch_unlikely(&rv_evl_enabled_key))
		return;
	on_each_cpu(reset_ipi, NULL, 1);
}

/* ---- per-IRQ handler-duration sidecar (separate from the DFA) ---- */

struct hwirq_snap_args {
	struct rv_hwirq_stat	*out; /* RV_EVL_HWIRQ_MAX entries */
};

static void hwirq_snapshot_ipi(void *info)
{
	struct hwirq_snap_args *a = info;
	struct rv_evl_pcpu *p = this_cpu_ptr(&rv_evl_pcpu);

	memcpy(a->out, p->hwirq, sizeof(p->hwirq));
}

static void hwirq_snapshot_cpu(unsigned int cpu, struct rv_hwirq_stat *out)
{
	struct hwirq_snap_args a = { .out = out };

	smp_call_function_single(cpu, hwirq_snapshot_ipi, &a, 1);
}

static int hwirq_stats_show(struct seq_file *seq, void *v)
{
	struct rv_hwirq_stat *snap;
	unsigned int irq;
	int cpu;

	snap = kcalloc(RV_EVL_HWIRQ_MAX, sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;

	seq_puts(seq, "# cpu,irq,name,count,max_ns,sum_ns,mean_ns\n");

	for_each_online_cpu(cpu) {
		hwirq_snapshot_cpu(cpu, snap);
		/*
		 * Walk irqdesc and ->action under rcu_read_lock: a
		 * concurrent free_irq() can unlink the action list and
		 * kfree(action) while we are dereferencing it from a
		 * tracefs read.  irq_to_desc() returns an RCU-protected
		 * pointer; copy the name into a local buffer before
		 * dropping the lock to avoid a seq_printf with a dangling
		 * pointer.
		 */
		for (irq = 0; irq < RV_EVL_HWIRQ_MAX; irq++) {
			struct rv_hwirq_stat *h = &snap[irq];
			char name[64] = "?";
			struct irq_desc *desc;
			struct irqaction *act;
			u64 mean = 0;

			if (!h->count)
				continue;

			rcu_read_lock();
			desc = irq_to_desc(irq);
			if (desc) {
				act = rcu_dereference(desc->action);
				if (act && act->name)
					strscpy(name, act->name, sizeof(name));
			}
			rcu_read_unlock();

			mean = div64_u64(h->sum_ns, h->count);
			seq_printf(seq, "%d,%u,%s,%llu,%llu,%llu,%llu\n",
				   cpu, irq, name,
				   (unsigned long long)h->count,
				   (unsigned long long)h->max_ns,
				   (unsigned long long)h->sum_ns,
				   (unsigned long long)mean);
		}
	}

	kfree(snap);

	seq_puts(seq, "#\n# Longest inband-irqoff blocker per CPU:\n");
	seq_puts(seq, "# cpu,longest_ns,caller\n");
	for_each_online_cpu(cpu) {
		struct rv_evl_pcpu *p = per_cpu_ptr(&rv_evl_pcpu, cpu);
		seq_printf(seq, "# %d,%llu,%pS\n", cpu,
			   (unsigned long long)p->longest_irqoff_ns,
			   (void *)p->longest_irqoff_caller);
	}

	return 0;
}

static int hwirq_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, hwirq_stats_show, inode->i_private);
}

static const struct file_operations hwirq_stats_fops = {
	.open		= hwirq_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- rv_register_monitor glue ---- */

static int rv_evl_enable(void)
{
	int ret;

	ret = rv_evl_attach();
	if (ret)
		return ret;
	static_branch_enable(&rv_evl_enabled_key);
	/*
	 * Discard the enable transient. Both static_branch_enable() calls
	 * above patch the keys via stop_machine(), which parks every CPU
	 * with hard IRQs off for up to a tick; once rv_irqoff_key is live
	 * that window is measured as a bogus multi-ms inband blocker. Now
	 * that all keys are patched and IRQs are back on, re-zero the
	 * per-CPU stats and resync the FSM so the first reported numbers
	 * reflect real activity only.
	 */
	on_each_cpu(reset_ipi, NULL, 1);
	return 0;
}

static void rv_evl_disable(void)
{
	static_branch_disable(&rv_evl_enabled_key);
	rv_evl_detach();
}

static void rv_evl_snapshot_edge_generic(unsigned int cpu, unsigned int edge,
					 void *out)
{
	rv_evl_snapshot_edge(cpu, edge, (struct rv_edge_stat *)out);
}

static struct rv_monitor rv_evl_monitor = {
	.name		    = "rv_evl",
	.description	    = "EVL OOB blocking edge-stat monitor",
	.enable		    = rv_evl_enable,
	.disable	    = rv_evl_disable,
	.reset		    = rv_evl_reset,
	.n_edges	    = NR_EDGES,
	.edge_labels	    = rv_evl_edge_labels,
	.snapshot_edge	    = rv_evl_snapshot_edge_generic,
	.coverage_snapshot  = rv_evl_coverage_snapshot,
	.extra_fops	    = &hwirq_stats_fops,
	.extra_name	    = "hwirq_stats",
};

static int __init rv_evl_init(void)
{
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		struct rv_evl_pcpu *p = per_cpu_ptr(&rv_evl_pcpu, cpu);

		p->state = RV_EVL_INITIAL;
		p->state_entered_at = 0;
	}

	ret = rv_register_monitor(&rv_evl_monitor, NULL);
	if (ret) {
		pr_warn("rv_evl: rv_register_monitor failed (%d)\n", ret);
		return ret;
	}

	pr_info("rv_evl: composed OOB blocking monitor ready (disabled), "
		"%u states / %u edges\n",
		RV_EVL_NR_STATES, NR_EDGES);
	return 0;
}

late_initcall(rv_evl_init);
