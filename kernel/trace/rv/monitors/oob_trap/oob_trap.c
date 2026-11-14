// SPDX-License-Identifier: GPL-2.0
/*
 * oob_trap: out-of-band trap notifications are balanced and keep the
 * Dovetail trap contract:
 *  - an exception the arch code reports as taken out-of-band
 *    (dovetail_trap_entry()) and must notify is notified next, with
 *    its trap number (oob_trap_must_notify());
 *  - every notification is unwound once, for the same trap, by the
 *    handler level which notified it;
 *  - the core demotes the task at most once per trap, and promotes it
 *    again only after the unwind;
 *  - the core's trap exit handler returns with hard irqs off;
 *  - a trap ends within timeout_ms (0: unbounded);
 *  - a task does not return to user mode with a notification open;
 *  - no interrupt is taken in NMI context (hard irqs on there);
 *  - no task is demoted from interrupt or NMI context;
 *  - a task does not schedule out while a kprobe is being handled.
 *
 * Copyright (C) 2026 Siemens AG
 * Author: Tobias Schaffner <tobias.schaffner@siemens.com>
 */
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <linux/kprobes.h>
#include <linux/hardirq.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "oob_trap"

#include <trace/events/dovetail.h>
#include <trace/events/sched.h>
#include <trace/events/irq.h>
#include <rv_trace.h>
#include <monitors/dovetail/dovetail.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "oob_trap.h"
#include "oob_trap_arch.h"
#include <rv/ha_monitor.h>

/*
 * The trap tracepoints fire with hard IRQs off, also from NMI context where
 * some arches (riscv) notify kernel mode traps. A trap is identified by its
 * number, its register frame and the stack frame of the arch code issuing
 * the call: a nested handler unwinding the trap of its caller has the same
 * number and registers, but a deeper stack frame. On 64-bit, kernel
 * addresses share their top byte.
 */
static DEFINE_PER_CPU(u64, oob_trap_key);

/*
 * The trap number the arch code reported on exception entry, and the
 * one it notified then. Both happen on the same CPU, with hard irqs off.
 */
static DEFINE_PER_CPU(unsigned int, oob_trap_expected);
static DEFINE_PER_CPU(unsigned int, oob_trap_nr);

/*
 * Arming a timer is not safe out-of-band or in NMI, so the clock is checked
 * on the task's next event instead; irq_entry ticks while the trap runs.
 * shortcut: a trap blocked in the kernel is caught only once it runs again,
 * use a timer once the hybrid automaton core can arm one out-of-band.
 */
static unsigned int timeout_ms = 1000;
module_param(timeout_ms, uint, 0644);

static u64 oob_trap_key_of(unsigned int trapnr, struct pt_regs *regs,
			   unsigned long frame)
{
	return ((u64)trapnr << 56) ^ (unsigned long)regs ^ frame;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_oob_trap env, u64 time_ns)
{
	if (env == same_trap_oob_trap)
		return READ_ONCE(ha_mon->env_store[env]) == __this_cpu_read(oob_trap_key);
	if (env == clk_oob_trap)	/* NMI safe, unlike ktime_get_ns(). */
		return ha_get_clk_ns(ha_mon, env, ktime_get_mono_fast_ns());
	if (env == irqs_off_oob_trap)
		return hard_irqs_disabled();
	if (env == no_kprobe_oob_trap)
		return kprobe_running() == NULL;
	if (env == task_ctx_oob_trap)
		return !in_nmi() && !on_pipeline_entry();
	if (env == not_nmi_oob_trap)
		return !in_nmi();
	if (env == right_trap_oob_trap)
		return __this_cpu_read(oob_trap_expected) == __this_cpu_read(oob_trap_nr);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_oob_trap env, u64 time_ns)
{
	if (env == same_trap_oob_trap)
		WRITE_ONCE(ha_mon->env_store[env], __this_cpu_read(oob_trap_key));
	if (env == clk_oob_trap)
		ha_reset_clk_ns(ha_mon, env, ktime_get_mono_fast_ns());
}

static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, u64 time_ns)
{
	u64 timeout_ns = (u64)READ_ONCE(timeout_ms) * NSEC_PER_MSEC;

	return curr_state == idle_oob_trap || !timeout_ns ||
	       ha_get_env(ha_mon, clk_oob_trap, time_ns) < timeout_ns;
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	switch (event) {
	case trap_entry_oob_trap:
		/* Nested in a notified trap: that one's clock keeps running. */
		if (curr_state == idle_oob_trap)
			ha_reset_env(ha_mon, clk_oob_trap, time_ns);
		return true;
	case trap_notify_oob_trap:
		ha_reset_env(ha_mon, same_trap_oob_trap, time_ns);
		ha_reset_env(ha_mon, clk_oob_trap, time_ns);
		return curr_state != entered_oob_trap ||
		       ha_get_env(ha_mon, right_trap_oob_trap, time_ns) == 1ull;
	case trap_unwind_oob_trap:
		return ha_monitor_env_invalid(ha_mon, same_trap_oob_trap) ||
		       ha_get_env(ha_mon, same_trap_oob_trap, time_ns) == 1ull;
	case trap_exit_kernel_oob_trap:
	case trap_exit_user_oob_trap:
		return ha_get_env(ha_mon, irqs_off_oob_trap, time_ns) == 1ull;
	case leave_oob_oob_trap:
		return ha_get_env(ha_mon, task_ctx_oob_trap, time_ns) == 1ull;
	case sched_switch_oob_trap:
		return ha_get_env(ha_mon, no_kprobe_oob_trap, time_ns) == 1ull;
	case irq_entry_oob_trap:
		return ha_get_env(ha_mon, not_nmi_oob_trap, time_ns) == 1ull;
	default:
		return true;
	}
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, time_ns))
		return false;

	return ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns);
}

static void report_trap_entry(int trapnr)
{
	__this_cpu_write(oob_trap_expected, trapnr);
	da_handle_start_run_event(current, trap_entry_oob_trap);
}

static void handle_trap_entry(void *data, unsigned long syndrome, struct pt_regs *regs)
{
	int trapnr = oob_trap_must_notify(syndrome, regs);

	if (trapnr >= 0)
		report_trap_entry(trapnr);
}

#ifdef CONFIG_X86
/* Page faults on user addresses must be notified, in either mode. */
static void handle_page_fault(void *data, unsigned long address,
			      struct pt_regs *regs, unsigned long error_code)
{
	if (running_oob() && !fault_in_kernel_space(address))
		report_trap_entry(X86_TRAP_PF);
}
#endif

static void handle_trap_notify(void *data, unsigned int trapnr, struct pt_regs *regs,
			       unsigned long frame)
{
	__this_cpu_write(oob_trap_nr, trapnr);
	__this_cpu_write(oob_trap_key, oob_trap_key_of(trapnr, regs, frame));
	da_handle_start_run_event(current, trap_notify_oob_trap);
}

static void handle_trap_unwind(void *data, unsigned int trapnr, struct pt_regs *regs,
			       unsigned long frame)
{
	__this_cpu_write(oob_trap_key, oob_trap_key_of(trapnr, regs, frame));
	da_handle_event(current, trap_unwind_oob_trap);
}

static void handle_trap_exit(void *data, unsigned int trapnr, struct pt_regs *regs)
{
	da_handle_event(current, user_mode(regs) ? trap_exit_user_oob_trap :
			trap_exit_kernel_oob_trap);
}

static void handle_oob_return(void *data, struct task_struct *p)
{
	da_handle_event(p, oob_return_oob_trap);
}

static void handle_irq_entry(void *data, int irq)
{
	da_handle_event(current, irq_entry_oob_trap);
}

static void handle_leave_oob(void *data, struct task_struct *p)
{
	da_handle_start_run_event(p, leave_oob_oob_trap);
}

/* Only tasks seen trapping or switching stage are monitored. */
static void handle_sched_switch(void *data, bool preempt, struct task_struct *prev,
				struct task_struct *next, unsigned int prev_state)
{
	da_handle_event(prev, sched_switch_oob_trap);
}

static void handle_leave_inband(void *data, struct task_struct *p)
{
	da_handle_start_run_event(p, leave_inband_oob_trap);
}

static int enable_oob_trap(void)
{
	int retval;

	retval = ha_monitor_init();
	if (retval)
		return retval;

	rv_attach_trace_probe("oob_trap", dovetail_trap_entry_tp, handle_trap_entry);
	rv_attach_trace_probe("oob_trap", dovetail_trap_notify_tp, handle_trap_notify);
	rv_attach_trace_probe("oob_trap", dovetail_trap_unwind_tp, handle_trap_unwind);
	rv_attach_trace_probe("oob_trap", dovetail_leave_inband_tp, handle_leave_inband);
	rv_attach_trace_probe("oob_trap", dovetail_trap_exit_tp, handle_trap_exit);
	rv_attach_trace_probe("oob_trap", dovetail_leave_oob_tp, handle_leave_oob);
	rv_attach_trace_probe("oob_trap", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("oob_trap", dovetail_oob_return_tp, handle_oob_return);
	rv_attach_trace_probe("oob_trap", irq_pipeline_entry, handle_irq_entry);
#ifdef CONFIG_X86
	rv_attach_trace_probe("oob_trap", page_fault_user, handle_page_fault);
	rv_attach_trace_probe("oob_trap", page_fault_kernel, handle_page_fault);
#endif

	return 0;
}

static void disable_oob_trap(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("oob_trap", dovetail_trap_entry_tp, handle_trap_entry);
	rv_detach_trace_probe("oob_trap", dovetail_trap_notify_tp, handle_trap_notify);
	rv_detach_trace_probe("oob_trap", dovetail_trap_unwind_tp, handle_trap_unwind);
	rv_detach_trace_probe("oob_trap", dovetail_leave_inband_tp, handle_leave_inband);
	rv_detach_trace_probe("oob_trap", dovetail_trap_exit_tp, handle_trap_exit);
	rv_detach_trace_probe("oob_trap", dovetail_leave_oob_tp, handle_leave_oob);
	rv_detach_trace_probe("oob_trap", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("oob_trap", dovetail_oob_return_tp, handle_oob_return);
	rv_detach_trace_probe("oob_trap", irq_pipeline_entry, handle_irq_entry);
#ifdef CONFIG_X86
	rv_detach_trace_probe("oob_trap", page_fault_user, handle_page_fault);
	rv_detach_trace_probe("oob_trap", page_fault_kernel, handle_page_fault);
#endif

	ha_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "oob_trap",
	.description = "out-of-band traps keep the Dovetail trap contract",
	.enable = enable_oob_trap,
	.disable = disable_oob_trap,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_oob_trap(void)
{
	return rv_register_monitor(&rv_this, &rv_dovetail);
}

static void __exit unregister_oob_trap(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_oob_trap);
module_exit(unregister_oob_trap);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobias Schaffner <tobias.schaffner@siemens.com>");
MODULE_DESCRIPTION("oob_trap: out-of-band traps keep the Dovetail trap contract");
