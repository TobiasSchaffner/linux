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
	demoted_oob_trap,
	unwinding_oob_trap,
	unwinding_demoted_oob_trap,
	entered_oob_trap,
	state_max_oob_trap,
};

#define INVALID_STATE state_max_oob_trap

enum events_oob_trap {
	irq_entry_oob_trap,
	leave_inband_oob_trap,
	leave_oob_oob_trap,
	oob_return_oob_trap,
	sched_switch_oob_trap,
	trap_entry_oob_trap,
	trap_exit_kernel_oob_trap,
	trap_exit_user_oob_trap,
	trap_notify_oob_trap,
	trap_unwind_oob_trap,
	event_max_oob_trap,
};

enum envs_oob_trap {
	same_trap_oob_trap,
	clk_oob_trap,
	irqs_off_oob_trap,
	no_kprobe_oob_trap,
	not_nmi_oob_trap,
	task_ctx_oob_trap,
	demotes_user_oob_trap,
	right_trap_oob_trap,
	env_max_oob_trap,
	env_max_stored_oob_trap = irqs_off_oob_trap,
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
		"demoted",
		"unwinding",
		"unwinding_demoted",
		"entered",
	},
	.event_names = {
		"irq_entry",
		"leave_inband",
		"leave_oob",
		"oob_return",
		"sched_switch",
		"trap_entry",
		"trap_exit_kernel",
		"trap_exit_user",
		"trap_notify",
		"trap_unwind",
	},
	.env_names = {
		"same_trap",
		"clk",
		"irqs_off",
		"no_kprobe",
		"not_nmi",
		"task_ctx",
		"demotes_user",
		"right_trap",
	},
	.function = {
		{ idle_oob_trap, idle_oob_trap, idle_oob_trap, idle_oob_trap, idle_oob_trap, entered_oob_trap, INVALID_STATE, INVALID_STATE, trapped_oob_trap, INVALID_STATE },
		{ trapped_oob_trap, INVALID_STATE, demoted_oob_trap, INVALID_STATE, trapped_oob_trap, trapped_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, unwinding_oob_trap },
		{ demoted_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, demoted_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, INVALID_STATE, unwinding_demoted_oob_trap },
		{ unwinding_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, unwinding_oob_trap, unwinding_oob_trap, idle_oob_trap, idle_oob_trap, INVALID_STATE, INVALID_STATE },
		{ unwinding_demoted_oob_trap, unwinding_oob_trap, INVALID_STATE, INVALID_STATE, unwinding_demoted_oob_trap, INVALID_STATE, idle_oob_trap, idle_oob_trap, INVALID_STATE, INVALID_STATE },
		{ entered_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, entered_oob_trap, INVALID_STATE, INVALID_STATE, INVALID_STATE, trapped_oob_trap, INVALID_STATE },
	},
	.initial_state = idle_oob_trap,
	.final_states = { 1, 0, 0, 0, 0, 0 },
};
