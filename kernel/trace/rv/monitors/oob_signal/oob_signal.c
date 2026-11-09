// SPDX-License-Identifier: GPL-2.0
/*
 * oob_signal: a signaled task does not return to user mode out-of-band.
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

#define MODULE_NAME "oob_signal"

#include <trace/events/dovetail.h>
#include <rv_trace.h>
#include <monitors/dovetail/dovetail.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "oob_signal.h"
#include <rv/da_monitor.h>

/*
 * No locking: the core sets TIF_MAYDAY before the notification moves the task
 * to signaled, and a return reads TIF_MAYDAY only after seeing signaled.
 */
static void handle_leave_inband(void *data, struct task_struct *p)
{
	da_handle_start_run_event(p, leave_inband_oob_signal);
	/* A signal notified before the switch is seen here, a later one finds oob. */
	if (task_sigpending(p))
		da_handle_event(p, signal_oob_signal);
}

static void handle_leave_oob(void *data, struct task_struct *p)
{
	da_handle_event(p, leave_oob_oob_signal);
}

static void handle_oob_return(void *data, struct task_struct *p)
{
	struct da_monitor *da_mon = da_get_monitor(p);

	if (!da_monitoring(da_mon))
		return;
	/* Pairs with the cmpxchg of the notification moving to signaled. */
	if (smp_load_acquire(&da_mon->curr_state) != signaled_oob_signal)
		return;
	da_handle_event(p, test_ti_thread_flag(task_thread_info(p), TIF_MAYDAY) ?
			oob_return_mayday_oob_signal : oob_return_oob_signal);
}

static void handle_signal_notify(void *data, struct task_struct *p)
{
	da_handle_event(p, signal_oob_signal);
}

static int enable_oob_signal(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	rv_attach_trace_probe("oob_signal", dovetail_leave_inband_tp, handle_leave_inband);
	rv_attach_trace_probe("oob_signal", dovetail_leave_oob_tp, handle_leave_oob);
	rv_attach_trace_probe("oob_signal", dovetail_oob_return_tp, handle_oob_return);
	rv_attach_trace_probe("oob_signal", dovetail_signal_notify_tp, handle_signal_notify);

	return 0;
}

static void disable_oob_signal(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("oob_signal", dovetail_leave_inband_tp, handle_leave_inband);
	rv_detach_trace_probe("oob_signal", dovetail_leave_oob_tp, handle_leave_oob);
	rv_detach_trace_probe("oob_signal", dovetail_oob_return_tp, handle_oob_return);
	rv_detach_trace_probe("oob_signal", dovetail_signal_notify_tp, handle_signal_notify);

	da_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "oob_signal",
	.description = "a signaled task does not return to user mode out-of-band",
	.enable = enable_oob_signal,
	.disable = disable_oob_signal,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_oob_signal(void)
{
	return rv_register_monitor(&rv_this, &rv_dovetail);
}

static void __exit unregister_oob_signal(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_oob_signal);
module_exit(unregister_oob_signal);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobias Schaffner <tobias.schaffner@siemens.com>");
MODULE_DESCRIPTION("oob_signal: a signaled task does not return to user mode out-of-band");
