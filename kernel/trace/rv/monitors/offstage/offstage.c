// SPDX-License-Identifier: GPL-2.0
/*
 * offstage: Linux leaves a task alone while it is out-of-band.
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
#include <linux/dovetail.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "offstage"

#include <trace/events/dovetail.h>
#include <trace/events/sched.h>
#include <trace/events/signal.h>
#include <rv_trace.h>
#include <monitors/dovetail/dovetail.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "offstage.h"
#include <rv/da_monitor.h>

static bool offstage_monitors_task(struct task_struct *p)
{
	return test_ti_local_flags(task_thread_info(p), _TLF_DOVETAIL);
}

static void handle_leave_inband(void *data, struct task_struct *p)
{
	da_handle_start_run_event(p, leave_inband_offstage);
}

static void handle_leave_oob(void *data, struct task_struct *p)
{
	da_handle_event(p, leave_oob_offstage);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	if (offstage_monitors_task(next))
		da_handle_event(next, sched_in_offstage);
}

static void handle_signal_generate(void *data, int sig,
				   struct kernel_siginfo *info,
				   struct task_struct *task, int group,
				   int result)
{
	if (task == current && offstage_monitors_task(task))
		da_handle_event(task, signal_self_offstage);
}

static int enable_offstage(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	rv_attach_trace_probe("offstage", dovetail_leave_inband_tp, handle_leave_inband);
	rv_attach_trace_probe("offstage", dovetail_leave_oob_tp, handle_leave_oob);
	rv_attach_trace_probe("offstage", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("offstage", signal_generate, handle_signal_generate);

	return 0;
}

static void disable_offstage(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("offstage", dovetail_leave_inband_tp, handle_leave_inband);
	rv_detach_trace_probe("offstage", dovetail_leave_oob_tp, handle_leave_oob);
	rv_detach_trace_probe("offstage", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("offstage", signal_generate, handle_signal_generate);

	da_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "offstage",
	.description = "Linux leaves a task alone while it is out-of-band",
	.enable = enable_offstage,
	.disable = disable_offstage,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_offstage(void)
{
	return rv_register_monitor(&rv_this, &rv_dovetail);
}

static void __exit unregister_offstage(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_offstage);
module_exit(unregister_offstage);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobias Schaffner <tobias.schaffner@siemens.com>");
MODULE_DESCRIPTION("offstage: Linux leaves a task alone while it is out-of-band");
