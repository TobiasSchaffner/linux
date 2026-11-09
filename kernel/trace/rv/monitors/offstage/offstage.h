/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of offstage automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME offstage

enum states_offstage {
	inband_offstage,
	oob_offstage,
	state_max_offstage,
};

#define INVALID_STATE state_max_offstage

enum events_offstage {
	leave_inband_offstage,
	leave_oob_offstage,
	sched_in_offstage,
	signal_self_offstage,
	event_max_offstage,
};

struct automaton_offstage {
	char *state_names[state_max_offstage];
	char *event_names[event_max_offstage];
	unsigned char function[state_max_offstage][event_max_offstage];
	unsigned char initial_state;
	bool final_states[state_max_offstage];
};

static const struct automaton_offstage automaton_offstage = {
	.state_names = {
		"inband",
		"oob",
	},
	.event_names = {
		"leave_inband",
		"leave_oob",
		"sched_in",
		"signal_self",
	},
	.function = {
		{
			oob_offstage,
			INVALID_STATE,
			inband_offstage,
			inband_offstage,
		},
		{
			INVALID_STATE,
			inband_offstage,
			INVALID_STATE,
			INVALID_STATE,
		},
	},
	.initial_state = inband_offstage,
	.final_states = { 1, 0 },
};
