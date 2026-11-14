/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The trap number the arch code must notify for an exception it reported
 * with dovetail_trap_entry(), -1 if it may handle the exception over the
 * oob stage without telling. Called from the tracepoint, i.e. on exception
 * entry, before the handler runs.
 *
 * shortcut: this mirrors which classes the arch handlers notify, update it
 * along with them.
 */
#ifdef CONFIG_ARM64
#include <asm/esr.h>
#include <linux/processor.h>

/*
 * Syscalls, system register emulation, external aborts, kernel mode
 * undefined instructions... are handled without telling. Kernel mode
 * aborts count on user addresses only, the exception table fixes up the
 * others in place.
 */
static int oob_trap_must_notify(unsigned long esr, struct pt_regs *regs)
{
	bool user = user_mode(regs);

	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_DABT_LOW:
	case ESR_ELx_EC_IABT_LOW:
	case ESR_ELx_EC_DABT_CUR:
	case ESR_ELx_EC_IABT_CUR:
		if (!user && !is_ttbr0_addr(read_sysreg(far_el1)))
			return -1;
		if (esr_fsc_is_translation_fault(esr) ||
		    esr_fsc_is_permission_fault(esr) ||
		    esr_fsc_is_access_flag_fault(esr) ||
		    (esr & ESR_ELx_FSC) == 0x21)	/* alignment fault */
			return ARM64_TRAP_ACCESS;
		return -1;
	case ESR_ELx_EC_SP_ALIGN:
	case ESR_ELx_EC_PC_ALIGN:
		return ARM64_TRAP_ALIGN;
	case ESR_ELx_EC_UNKNOWN:
		return user ? ARM64_TRAP_UNDI : -1;
	case ESR_ELx_EC_BTI:
		return user ? ARM64_TRAP_BTI : -1;
	case ESR_ELx_EC_FP_EXC64:
	case ESR_ELx_EC_FP_EXC32:
		return ARM64_TRAP_FPE;
	case ESR_ELx_EC_SVE:
		return ARM64_TRAP_SVE;
	case ESR_ELx_EC_SME:
		return ARM64_TRAP_SME;
	case ESR_ELx_EC_BREAKPT_LOW:
	case ESR_ELx_EC_BREAKPT_CUR:
	case ESR_ELx_EC_SOFTSTP_LOW:
	case ESR_ELx_EC_SOFTSTP_CUR:
	case ESR_ELx_EC_WATCHPT_LOW:
	case ESR_ELx_EC_WATCHPT_CUR:
	case ESR_ELx_EC_BRK64:
	case ESR_ELx_EC_BKPT32:
		return ARM64_TRAP_DEBUG;
	default:
		return -1;
	}
}
#elif defined(CONFIG_X86)
#include <asm/traps.h>
#include <asm/trapnr.h>
#include <trace/events/exceptions.h>

/*
 * The arch code reports the trap number of exceptions it may have to
 * notify: user mode ones and breakpoints. Page faults come from the
 * page_fault_{user,kernel} events.
 */
static int oob_trap_must_notify(unsigned long trapnr, struct pt_regs *regs)
{
	return user_mode(regs) || trapnr == X86_TRAP_BP ? trapnr : -1;
}
#else
/* The arch code reports the trap number, and only for traps to notify. */
static int oob_trap_must_notify(unsigned long trapnr, struct pt_regs *regs)
{
	return trapnr;
}
#endif
