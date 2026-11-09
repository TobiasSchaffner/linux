/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dovetail

#if !defined(_TRACE_DOVETAIL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DOVETAIL_H

#include <linux/tracepoint.h>

struct task_struct;

DECLARE_TRACE(dovetail_leave_inband,
	TP_PROTO(struct task_struct *p),
	TP_ARGS(p));

DECLARE_TRACE(dovetail_leave_oob,
	TP_PROTO(struct task_struct *p),
	TP_ARGS(p));

#endif /* _TRACE_DOVETAIL_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
