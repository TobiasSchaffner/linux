/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Runtime-Verification IRQ-off accounting hook for the EVL edge-stat
 * monitor.
 *
 * Provides a pair of __always_inline notrace hooks that are wired
 * into the EVL/Dovetail hard-IRQ disable/enable hot path
 * (include/asm-generic/irq_pipeline.h). The hooks are gated by a
 * single static_branch and compile to a patched NOP when
 * CONFIG_RV_MON_EVL is enabled but the runtime switch is OFF
 * (default).
 *
 * The actual statistics update lives out-of-line in
 * kernel/trace/rv/monitors/evl/evl.c so this header stays small and
 * include-cycle safe (it only pulls in jump_label.h).
 *
 * Semantics:
 *   - rv_irqoff_account_off() is called *after* hard IRQs have been
 *     disabled (so the accounting itself runs atomically).
 *   - rv_irqoff_account_on()  is called *before* hard IRQs are enabled
 *     (same reason).
 *
 * Copyright (C) 2026 Siemens AG
 * Author:       Tobias Schaffner <tobias.schaffner@siemens.com>.
 */
#ifndef _LINUX_RV_IRQOFF_H
#define _LINUX_RV_IRQOFF_H

#include <linux/types.h>

#ifdef CONFIG_RV_MON_EVL

#include <linux/compiler.h>
#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(rv_irqoff_key);

void __rv_irqoff_off(void) __attribute__((no_instrument_function));
void __rv_irqoff_on(void)  __attribute__((no_instrument_function));

/*
 * Compile-out / patch-out gate. Use as the *first* term of a
 * short-circuit expression in the irq_pipeline.h macros so that the
 * subsequent edge-detect predicate (which reads CSRs on RISC-V) is
 * elided entirely when the runtime switch is OFF.
 */
static __always_inline __attribute__((no_instrument_function))
bool rv_irqoff_enabled(void)
{
	return static_branch_unlikely(&rv_irqoff_key);
}

static __always_inline __attribute__((no_instrument_function))
void rv_irqoff_account_off(void)
{
	if (rv_irqoff_enabled())
		__rv_irqoff_off();
}

static __always_inline __attribute__((no_instrument_function))
void rv_irqoff_account_on(void)
{
	if (rv_irqoff_enabled())
		__rv_irqoff_on();
}

/*
 * Record a per-CPU timestamp at trap arrival, regardless of whether
 * hard IRQs were enabled before the trap. Lets the monitor split edge 4
 * dwell (hwirq_enter_over_irqoff_inband) into blocker and dispatch.
 */
void __rv_evl_account_trap_arrival(void) __attribute__((no_instrument_function));

static __always_inline __attribute__((no_instrument_function))
void rv_evl_account_trap_arrival(void)
{
	if (rv_irqoff_enabled())
		__rv_evl_account_trap_arrival();
}

/*
 * Read the last trap-arrival timestamp for the current CPU in the
 * CLOCK_MONOTONIC (ktime_get_mono_fast_ns) timebase, as stamped by the
 * rv_evl monitor on the trap-entry path. Returns 0 when the monitor's
 * runtime switch is OFF, so callers can detect "monitor not running"
 * and skip correlation. Out-of-line in evl.c.
 */
u64 rv_evl_trap_arrival_mono(void) __attribute__((no_instrument_function));

/*
 * Read the current CPU's last wake-path inband-irqoff blocker (ns), as
 * computed by the rv_evl monitor at hwirq entry: the time a trap waited
 * because the inband stage held hard IRQs off, or 0 if it arrived with
 * IRQs enabled. Returns 0 when the monitor is off. Out-of-line in evl.c.
 */
u64 rv_evl_trap_blocker_ns(void) __attribute__((no_instrument_function));

#else  /* !CONFIG_RV_MON_EVL */

static inline bool rv_irqoff_enabled(void) { return false; }
static inline void __rv_irqoff_off(void) { }
static inline void __rv_irqoff_on(void)  { }
static inline void rv_irqoff_account_off(void) { }
static inline void rv_irqoff_account_on(void)  { }
static inline void rv_evl_account_trap_arrival(void) { }
static inline u64 rv_evl_trap_arrival_mono(void) { return 0; }
static inline u64 rv_evl_trap_blocker_ns(void) { return 0; }

#endif /* CONFIG_RV_MON_EVL */

#endif /* _LINUX_RV_IRQOFF_H */
