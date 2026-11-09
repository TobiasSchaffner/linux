/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Snippet to be included in rv_trace.h
 */

#ifdef CONFIG_RV_MON_OOB_SIGNAL
DEFINE_EVENT(event_da_monitor_id, event_oob_signal,
	     TP_PROTO(int id, char *state, char *event, char *next_state, bool final_state),
	     TP_ARGS(id, state, event, next_state, final_state));

DEFINE_EVENT(error_da_monitor_id, error_oob_signal,
	     TP_PROTO(int id, char *state, char *event),
	     TP_ARGS(id, state, event));
#endif /* CONFIG_RV_MON_OOB_SIGNAL */
