/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of oob_trap automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME oob_trap

enum states_oob_trap {
	idle_oob_trap,
	trapped_oob_trap,
	state_max_oob_trap,
};

#define INVALID_STATE state_max_oob_trap

enum events_oob_trap {
	leave_inband_oob_trap,
	trap_notify_oob_trap,
	trap_unwind_oob_trap,
	event_max_oob_trap,
};

enum envs_oob_trap {
	same_trap_oob_trap,
	env_max_oob_trap,
	env_max_stored_oob_trap = env_max_oob_trap,
};

_Static_assert(env_max_stored_oob_trap <= MAX_HA_ENV_LEN, "Not enough slots");

struct automaton_oob_trap {
	char *state_names[state_max_oob_trap];
	char *event_names[event_max_oob_trap];
	char *env_names[env_max_oob_trap];
	unsigned char function[state_max_oob_trap][event_max_oob_trap];
	unsigned char initial_state;
	bool final_states[state_max_oob_trap];
};

static const struct automaton_oob_trap automaton_oob_trap = {
	.state_names = {
		"idle",
		"trapped",
	},
	.event_names = {
		"leave_inband",
		"trap_notify",
		"trap_unwind",
	},
	.env_names = {
		"same_trap",
	},
	.function = {
		{          idle_oob_trap,       trapped_oob_trap,          INVALID_STATE },
		{          INVALID_STATE,          INVALID_STATE,          idle_oob_trap },
	},
	.initial_state = idle_oob_trap,
	.final_states = { 1, 0 },
};
