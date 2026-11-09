/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of oob_signal automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME oob_signal

enum states_oob_signal {
	inband_oob_signal,
	oob_oob_signal,
	signaled_oob_signal,
	state_max_oob_signal,
};

#define INVALID_STATE state_max_oob_signal

enum events_oob_signal {
	leave_inband_oob_signal,
	leave_oob_oob_signal,
	oob_return_oob_signal,
	oob_return_mayday_oob_signal,
	signal_oob_signal,
	event_max_oob_signal,
};

struct automaton_oob_signal {
	char *state_names[state_max_oob_signal];
	char *event_names[event_max_oob_signal];
	unsigned char function[state_max_oob_signal][event_max_oob_signal];
	unsigned char initial_state;
	bool final_states[state_max_oob_signal];
};

static const struct automaton_oob_signal automaton_oob_signal = {
	.state_names = {
		"inband",
		"oob",
		"signaled",
	},
	.event_names = {
		"leave_inband",
		"leave_oob",
		"oob_return",
		"oob_return_mayday",
		"signal",
	},
	.function = {
		{
			oob_oob_signal,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			inband_oob_signal,
		},
		{
			INVALID_STATE,
			inband_oob_signal,
			oob_oob_signal,
			oob_oob_signal,
			signaled_oob_signal,
		},
		{
			INVALID_STATE,
			inband_oob_signal,
			INVALID_STATE,
			signaled_oob_signal,
			signaled_oob_signal,
		},
	},
	.initial_state = inband_oob_signal,
	.final_states = { 1, 0, 0 },
};
