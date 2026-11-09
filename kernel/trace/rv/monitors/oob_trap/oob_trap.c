// SPDX-License-Identifier: GPL-2.0
/*
 * oob_trap: out-of-band trap notifications are balanced.
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
#include <rv/instrumentation.h>

#define MODULE_NAME "oob_trap"

#include <trace/events/dovetail.h>
#include <trace/events/sched.h>
#include <rv_trace.h>
#include <monitors/dovetail/dovetail.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "oob_trap.h"
#include <rv/ha_monitor.h>

/*
 * Both trap tracepoints fire with hard IRQs off, and NMIs are ignored. A trap
 * is identified by its number and frame; on 64-bit, kernel addresses share
 * their top byte.
 */
static DEFINE_PER_CPU(u64, oob_trap_key);

static u64 oob_trap_key_of(unsigned int trapnr, struct pt_regs *regs)
{
	return ((u64)trapnr << 56) ^ (unsigned long)regs;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_oob_trap env, u64 time_ns)
{
	if (env == same_trap_oob_trap)
		return READ_ONCE(ha_mon->env_store[env]) == __this_cpu_read(oob_trap_key);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_oob_trap env, u64 time_ns)
{
	if (env == same_trap_oob_trap)
		WRITE_ONCE(ha_mon->env_store[env], __this_cpu_read(oob_trap_key));
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == idle_oob_trap && event == trap_notify_oob_trap)
		ha_reset_env(ha_mon, same_trap_oob_trap, time_ns);
	else if (curr_state == trapped_oob_trap && event == trap_unwind_oob_trap)
		res = ha_monitor_env_invalid(ha_mon, same_trap_oob_trap) ||
		      ha_get_env(ha_mon, same_trap_oob_trap, time_ns) == 1ull;
	return res;
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	return ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns);
}

static void handle_trap_notify(void *data, unsigned int trapnr, struct pt_regs *regs)
{
	if (in_nmi())
		return;
	__this_cpu_write(oob_trap_key, oob_trap_key_of(trapnr, regs));
	da_handle_start_run_event(current, trap_notify_oob_trap);
}

static void handle_trap_unwind(void *data, unsigned int trapnr, struct pt_regs *regs)
{
	if (in_nmi())
		return;
	__this_cpu_write(oob_trap_key, oob_trap_key_of(trapnr, regs));
	da_handle_event(current, trap_unwind_oob_trap);
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

	rv_attach_trace_probe("oob_trap", dovetail_trap_notify_tp, handle_trap_notify);
	rv_attach_trace_probe("oob_trap", dovetail_trap_unwind_tp, handle_trap_unwind);
	rv_attach_trace_probe("oob_trap", dovetail_leave_inband_tp, handle_leave_inband);

	return 0;
}

static void disable_oob_trap(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("oob_trap", dovetail_trap_notify_tp, handle_trap_notify);
	rv_detach_trace_probe("oob_trap", dovetail_trap_unwind_tp, handle_trap_unwind);
	rv_detach_trace_probe("oob_trap", dovetail_leave_inband_tp, handle_leave_inband);

	ha_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "oob_trap",
	.description = "out-of-band trap notifications are balanced",
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
MODULE_DESCRIPTION("oob_trap: out-of-band trap notifications are balanced");
