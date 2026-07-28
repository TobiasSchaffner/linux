/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2024-2026 Siemens AG
 * Author:       Tobias Schaffner <tobias.schaffner@siemens.com>.
 */
#ifndef _ASM_RISCV_DOVETAIL_H
#define _ASM_RISCV_DOVETAIL_H

#if !defined(__ASSEMBLY__)
#ifdef CONFIG_DOVETAIL

static inline void arch_dovetail_exec_prepare(void)
{ }

static inline void arch_dovetail_switch_prepare(bool leave_inband)
{ }

static inline void arch_dovetail_switch_finish(bool enter_inband)
{ }

#endif /* CONFIG_DOVETAIL */
#endif /* !__ASSEMBLY__ */
#endif /* _ASM_RISCV_DOVETAIL_H */
