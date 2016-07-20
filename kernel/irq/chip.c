// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code, for irq-chip based
 * architectures. Detailed information is available in
 * Documentation/core-api/genericirq.rst
 */

#include <linux/irq.h>
#include <linux/irq_pipeline.h>
#include <linux/msi.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/irqdomain.h>
#include <linux/preempt.h>
#include <linux/random.h>

#include <trace/events/irq.h>

#include "internals.h"

static irqreturn_t bad_chained_irq(int irq, void *dev_id)
{
	WARN_ONCE(1, "Chained irq %d should not call an action\n", irq);
	return IRQ_NONE;
}

/*
 * Chained handlers should never call action on their IRQ. This default
 * action will emit warning if such thing happens.
 */
struct irqaction chained_action = {
	.handler = bad_chained_irq,
};

/**
 * irq_set_chip - set the irq chip for an irq
 * @irq:	irq number
 * @chip:	pointer to irq chip description structure
 */
int irq_set_chip(unsigned int irq, const struct irq_chip *chip)
{
	int ret = -EINVAL;

	WARN_ONCE(irqs_pipelined() && chip &&
		(chip->flags & IRQCHIP_PIPELINE_SAFE) == 0,
		"irqchip %s is not pipeline-safe!", chip->name);

	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_data.chip = (struct irq_chip *)(chip ?: &no_irq_chip);
		ret = 0;
	}
	if (!ret) {
		/* For !CONFIG_SPARSE_IRQ make the irq show up in allocated_irqs. */
		irq_mark_irq(irq);
		irq_proc_update_chip(chip);
	}
	return ret;
}
EXPORT_SYMBOL(irq_set_chip);

/**
 * irq_set_irq_type - set the irq trigger type for an irq
 * @irq:	irq number
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 */
int irq_set_irq_type(unsigned int irq, unsigned int type)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL)
		return __irq_set_trigger(scoped_irqdesc, type);
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_irq_type);

/**
 * irq_set_handler_data - set irq handler data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to interrupt specific data
 *
 * Set the hardware irq controller data for an irq
 */
int irq_set_handler_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_common_data.handler_data = data;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_handler_data);

/**
 * irq_set_msi_desc_off - set MSI descriptor data for an irq at offset
 * @irq_base:	Interrupt number base
 * @irq_offset:	Interrupt number offset
 * @entry:		Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq at offset
 */
int irq_set_msi_desc_off(unsigned int irq_base, unsigned int irq_offset, struct msi_desc *entry)
{
	scoped_irqdesc_get_and_lock(irq_base + irq_offset, IRQ_GET_DESC_CHECK_GLOBAL) {
		scoped_irqdesc->irq_common_data.msi_desc = entry;
		if (entry && !irq_offset)
			entry->irq = irq_base;
		return 0;
	}
	return -EINVAL;
}

/**
 * irq_set_msi_desc - set MSI descriptor data for an irq
 * @irq:	Interrupt number
 * @entry:	Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq
 */
int irq_set_msi_desc(unsigned int irq, struct msi_desc *entry)
{
	return irq_set_msi_desc_off(irq, 0, entry);
}

/**
 * irq_set_chip_data - set irq chip data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to chip specific data
 *
 * Set the hardware irq chip data for an irq
 */
int irq_set_chip_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_data.chip_data = data;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_chip_data);

struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? &desc->irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_get_irq_data);

static void irq_state_clr_disabled(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_DISABLED);
}

static void irq_state_clr_masked(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_MASKED);
}

static void irq_state_clr_started(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_STARTED);
}

static void irq_state_set_started(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_STARTED);
}

enum {
	IRQ_STARTUP_NORMAL,
	IRQ_STARTUP_MANAGED,
	IRQ_STARTUP_ABORT,
};

#ifdef CONFIG_SMP
static int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return IRQ_STARTUP_NORMAL;

	irqd_clr_managed_shutdown(d);

	if (!cpumask_intersects(aff, cpu_online_mask)) {
		/*
		 * Catch code which fiddles with enable_irq() on a managed
		 * and potentially shutdown IRQ. Chained interrupt
		 * installment or irq auto probing should not happen on
		 * managed irqs either.
		 */
		if (WARN_ON_ONCE(force))
			return IRQ_STARTUP_ABORT;
		/*
		 * The interrupt was requested, but there is no online CPU
		 * in it's affinity mask. Put it into managed shutdown
		 * state and let the cpu hotplug mechanism start it up once
		 * a CPU in the mask becomes available.
		 */
		return IRQ_STARTUP_ABORT;
	}
	/*
	 * Managed interrupts have reserved resources, so this should not
	 * happen.
	 */
	if (WARN_ON(irq_domain_activate_irq(d, false)))
		return IRQ_STARTUP_ABORT;
	return IRQ_STARTUP_MANAGED;
}

void irq_startup_managed(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	/*
	 * Clear managed-shutdown flag, so we don't repeat managed-startup for
	 * multiple hotplugs, and cause imbalanced disable depth.
	 */
	irqd_clr_managed_shutdown(d);

	/*
	 * Only start it up when the disable depth is 1, so that a disable,
	 * hotunplug, hotplug sequence does not end up enabling it during
	 * hotplug unconditionally.
	 */
	desc->depth--;
	if (!desc->depth)
		irq_startup(desc, IRQ_RESEND, IRQ_START_COND);
}

#else
static __always_inline int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	return IRQ_STARTUP_NORMAL;
}
#endif

static void irq_enable(struct irq_desc *desc)
{
	if (!irqd_irq_disabled(&desc->irq_data)) {
		unmask_irq(desc);
	} else {
		irq_state_clr_disabled(desc);
		if (desc->irq_data.chip->irq_enable) {
			desc->irq_data.chip->irq_enable(&desc->irq_data);
			irq_state_clr_masked(desc);
		} else {
			unmask_irq(desc);
		}
	}
}

static int __irq_startup(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	int ret = 0;

	/* Warn if this interrupt is not activated but try nevertheless */
	WARN_ON_ONCE(!irqd_is_activated(d));

	if (d->chip->irq_startup) {
		ret = d->chip->irq_startup(d);
		irq_state_clr_disabled(desc);
		irq_state_clr_masked(desc);
	} else {
		irq_enable(desc);
	}
	irq_state_set_started(desc);
	return ret;
}

int irq_startup(struct irq_desc *desc, bool resend, bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	const struct cpumask *aff = irq_data_get_affinity_mask(d);
	int ret = 0;

	desc->depth = 0;

	if (irqd_is_started(d)) {
		irq_enable(desc);
	} else {
		switch (__irq_startup_managed(desc, aff, force)) {
		case IRQ_STARTUP_NORMAL:
			if (d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP)
				irq_setup_affinity(desc);
			ret = __irq_startup(desc);
			if (!(d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP))
				irq_setup_affinity(desc);
			break;
		case IRQ_STARTUP_MANAGED:
			irq_do_set_affinity(d, aff, false);
			ret = __irq_startup(desc);
			break;
		case IRQ_STARTUP_ABORT:
			desc->depth = 1;
			irqd_set_managed_shutdown(d);
			return 0;
		}
	}
	if (resend)
		check_irq_resend(desc, false);

	return ret;
}

int irq_activate(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return irq_domain_activate_irq(d, false);
	return 0;
}

int irq_activate_and_startup(struct irq_desc *desc, bool resend)
{
	if (WARN_ON(irq_activate(desc)))
		return 0;
	return irq_startup(desc, resend, IRQ_START_FORCE);
}

static void __irq_disable(struct irq_desc *desc, bool mask);

void irq_shutdown(struct irq_desc *desc)
{
	if (irqd_is_started(&desc->irq_data)) {
		clear_irq_resend(desc);
		/*
		 * Increment disable depth, so that a managed shutdown on
		 * CPU hotunplug preserves the actual disabled state when the
		 * CPU comes back online. See irq_startup_managed().
		 */
		desc->depth++;

		if (desc->irq_data.chip->irq_shutdown) {
			desc->irq_data.chip->irq_shutdown(&desc->irq_data);
			irq_state_set_disabled(desc);
			irq_state_set_masked(desc);
		} else {
			__irq_disable(desc, true);
		}
		irq_state_clr_started(desc);
	}
}


void irq_shutdown_and_deactivate(struct irq_desc *desc)
{
	irq_shutdown(desc);
	/*
	 * This must be called even if the interrupt was never started up,
	 * because the activation can happen before the interrupt is
	 * available for request/startup. It has it's own state tracking so
	 * it's safe to call it unconditionally.
	 */
	irq_domain_deactivate_irq(&desc->irq_data);
}

static void __irq_disable(struct irq_desc *desc, bool mask)
{
	if (irqd_irq_disabled(&desc->irq_data)) {
		if (mask)
			mask_irq(desc);
	} else {
		irq_state_set_disabled(desc);
		if (desc->irq_data.chip->irq_disable) {
			desc->irq_data.chip->irq_disable(&desc->irq_data);
			irq_state_set_masked(desc);
		} else if (mask) {
			mask_irq(desc);
		}
	}
}

/**
 * irq_disable - Mark interrupt disabled
 * @desc:	irq descriptor which should be disabled
 *
 * If the chip does not implement the irq_disable callback, we
 * use a lazy disable approach. That means we mark the interrupt
 * disabled, but leave the hardware unmasked. That's an
 * optimization because we avoid the hardware access for the
 * common case where no interrupt happens after we marked it
 * disabled. If an interrupt happens, then the interrupt flow
 * handler masks the line at the hardware level and marks it
 * pending.
 *
 * If the interrupt chip does not implement the irq_disable callback,
 * a driver can disable the lazy approach for a particular irq line by
 * calling 'irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY)'. This can
 * be used for devices which cannot disable the interrupt at the
 * device level under certain circumstances and have to use
 * disable_irq[_nosync] instead.
 */
void irq_disable(struct irq_desc *desc)
{
	__irq_disable(desc, irq_settings_disable_unlazy(desc) || irqs_pipelined());
}

void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_enable)
		desc->irq_data.chip->irq_enable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
	cpumask_set_cpu(cpu, desc->percpu_enabled);
}

void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_disable)
		desc->irq_data.chip->irq_disable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_mask(&desc->irq_data);
	cpumask_clear_cpu(cpu, desc->percpu_enabled);
}

static inline void mask_ack_irq(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_mask_ack) {
		desc->irq_data.chip->irq_mask_ack(&desc->irq_data);
		irq_state_set_masked(desc);
	} else {
		mask_irq(desc);
		if (desc->irq_data.chip->irq_ack)
			desc->irq_data.chip->irq_ack(&desc->irq_data);
	}
}

void mask_irq(struct irq_desc *desc)
{
	if (irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_mask) {
		desc->irq_data.chip->irq_mask(&desc->irq_data);
		irq_state_set_masked(desc);
	}
}

void unmask_irq(struct irq_desc *desc)
{
	if (!irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_unmask) {
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
		irq_state_clr_masked(desc);
	}
}

void unmask_threaded_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	if (chip->flags & IRQCHIP_EOI_THREADED)
		chip->irq_eoi(&desc->irq_data);

	unmask_irq(desc);
}

enum {
	IRQ_FLOW_START,
	IRQ_FLOW_REPLAY,
	IRQ_FLOW_FORWARD,
};

#ifdef CONFIG_IRQ_PIPELINE

static inline bool may_start_flow(int flow)
{
	return flow == IRQ_FLOW_START;
}

void irq_clear_deferral(struct irq_desc *desc)
{
	desc->istate &= ~IRQS_DEFERRED;
}

void irq_clear_forward(struct irq_desc *desc)
{
	desc->istate &= ~IRQS_FORWARDED;
}

static irqreturn_t forward_irq_event(struct irq_desc *desc)
{
	irqreturn_t ret = IRQ_HANDLED;

	if (likely(running_inband())) {
		raw_spin_unlock(&desc->lock);
		ret = handle_irq_event_percpu(desc);
		raw_spin_lock(&desc->lock);
	}

	return ret;
}

#else  /* !CONFIG_IRQ_PIPELINE */

static inline bool may_start_flow(int flow)
{
	return true;
}

static irqreturn_t forward_irq_event(struct irq_desc *desc)
{
	return IRQ_NONE;
}

#endif	/* !CONFIG_IRQ_PIPELINE */

/*
 * get_flow_step - Determine which step of the interrupt flow handling
 * we are in.
 *
 * When interrupt pipelining is enabled, interrupt flow handlers may
 * be called twice for each event, following either of these patterns:
 *
 * - initially from the pipeline entry context in a attempt to handle
 *   the hardware event from the oob stage, next if the event was
 *   deferred to the in-band stage in absence of any oob handler. In
 *   the first case, we have to ack the hardware before feeding the
 *   pipeline. Otherwise, we should simply run the action associated
 *   to the event from the in-band stage. In this case, the IRQ action
 *   handler is called only once from the in-band stage.
 *
 * OR,
 *
 * - initially from the pipeline entry context for handling the
 *   hardware event from the oob stage. The action handler may then
 *   issue a forwarding request of the incoming event to the in-band
 *   stage once done. In this case, the same IRQ action handler is
 *   called twice, first from the oob stage then from the in-band
 *   stage. The IRQ handling code should account for the fact that
 *   multiple incoming events may be received on the oob stage from a
 *   given source before the forwarding handler is eventually called
 *   in-band only once by the pipeline core to cover all of them.
 *
 * When IRQ forwarding is requested by an oob action handler
 * (IRQS_FORWARDED is set in the per-descriptor state), acknowledge
 * was already sent to the hardware, therefore we want to skip this
 * part when running the in-band call.
 *
 * With stacked irqchips, handlers of parent irqs not delivered to the
 * oob stage would decode and post the cascaded events to the in-band
 * stage as well. The internal per-descriptor IRQS_DEFERRED flag
 * disambiguates between the latter context and events being replayed
 * after deferral to the in-band stage, so that we do ack the hardware
 * for cascaded events as expected. We might also receive an event
 * while a previous one is still pending replay or forwarding.
 *
 * The flow state enumeration below disambiguates all cases:
 *
 * - IRQ_FLOW_START: we are running oob on behalf of the low-level irq
 *   entry code, we need to send ack to the hardware. This includes
 *   the case when we receive a new event on any stage while an event
 *   of the same type is already pending replay or forwarding.
 *
 * - IRQ_FLOW_REPLAY: we are (re)playing a deferred event from the
 *   in-band stage, as a result of synchronizing the irq log. A
 *   deferred event was NOT handled from the oob stage by definition.
 *
 * - IRQ_FLOW_FORWARD: an event already handled from the oob stage by
 *   an action handler is forwarded to the same handler for execution
 *   on the in-band stage.  This mode allows for two-step handling of
 *   an IRQ event over both stages by the same action
 *   handler. IRQ_FLOW_FORWARD is mutually exclusive with
 *   IRQ_FLOW_REPLAY since only the oob handler may request
 *   forwarding.
 */
static inline int get_flow_step(struct irq_desc *desc)
{
	if (likely(!irqs_pipelined() || in_pipeline()))
		return IRQ_FLOW_START;

	/*
	 * We must be running in-band, synchronizing the interrupt log
	 * on the current CPU. Determine whether we are handling a
	 * deferred or forwarded interrupt.
	 */
	WARN_ON(irq_pipeline_debug() && running_oob());

	if (desc->istate & IRQS_FORWARDED)
		return IRQ_FLOW_FORWARD;

	if (desc->istate & IRQS_DEFERRED)
		return IRQ_FLOW_REPLAY;

	/*
	 * Rare case: disambiguate between forwarded IRQs and replayed
	 * ones for multiple non-oob events from the same source which
	 * don't need masking on receipt (e.g. edge IRQs), causing the
	 * replay condition bit to be cleared by
	 * should_feed_pipeline() on the first occurrence, leaving no
	 * information to handle the second one appropriately.  We can
	 * still figure out that a replay condition is pending
	 * nevertheless (instead of a forwarding) by checking the oob
	 * capability for the interrupt: oob handled IRQs visible to
	 * the in-band stage are never deferred (IRQS_DEFERRED), they
	 * can only be forwarded (IRQS_FORWARDED).
	 */
	return irq_settings_is_oob(desc) ? IRQ_FLOW_FORWARD : IRQ_FLOW_REPLAY;
}

/*
 * should_feed_pipeline - Determine whether we should be feeding the
 * interrupt pipeline with the incoming event.
 *
 * Depending on the flow state determined by get_flow_step(), the
 * event is passed to the oob handler or directly replayed from the
 * current in-band stage.
 */
static inline bool should_feed_pipeline(struct irq_desc *desc, int state)
{
	if (!irqs_pipelined())
		return false;

	switch (state) {
	case IRQ_FLOW_REPLAY:	/* Deferred to in-band. */
		irq_clear_deferral(desc);
		return false;
	case IRQ_FLOW_FORWARD:	/* Handled oob, forwarded in-band. */
		irq_clear_forward(desc);
		return false;
	default:
		return true;
	}
}

/* Busy wait until INPROGRESS is cleared */
static bool irq_wait_on_inprogress(struct irq_desc *desc)
{
	if (IS_ENABLED(CONFIG_SMP)) {
		do {
			raw_spin_unlock(&desc->lock);
			while (irqd_irq_inprogress(&desc->irq_data))
				cpu_relax();
			raw_spin_lock(&desc->lock);
		} while (irqd_irq_inprogress(&desc->irq_data));

		/* Might have been disabled in meantime */
		return !irqd_irq_disabled(&desc->irq_data) && desc->action;
	}
	return false;
}

#ifdef CONFIG_IRQ_PIPELINE
static bool irq_active_on_this_cpu(struct irq_desc *desc)
{
	struct irq_data *data = &desc->irq_data;
	const struct cpumask *aff;

	/* desc marked as in progress, no other CPUs on !SMP */
	if (!IS_ENABLED(CONFIG_SMP))
		return true;

	/* The below works only for single target interrupts */
	if (!irqd_is_single_target(data) || desc->handle_irq != handle_edge_irq)
		return false;

	aff = irq_data_get_effective_affinity_mask(data);
	if (cpumask_first(aff) != smp_processor_id())
		return false;

	return true;
}
#else
static inline bool irq_active_on_this_cpu(struct irq_desc *desc)
{
	return false;
}
#endif

static bool irq_can_handle_pm(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;
	const struct cpumask *aff;

	/*
	 * If the interrupt is not in progress and is not an armed
	 * wakeup interrupt, proceed.
	 */
	if (!irqd_has_set(irqd, IRQD_IRQ_INPROGRESS | IRQD_WAKEUP_ARMED))
		return true;

	/*
	 * If the interrupt is an armed wakeup source, mark it pending
	 * and suspended, disable it and notify the pm core about the
	 * event.
	 *
	 * When pipelining, the logic is as follows:
	 *
	 * - from a pipeline entry context, we might have preempted
	 * the oob stage, or irqs might be [virtually] off, so we may
	 * not run the in-band PM code. Just make sure any wakeup
	 * interrupt is detected later on when the flow handler
	 * re-runs from the in-band stage.
	 *
	 * - from the in-band context, run the PM wakeup check.
	 */
	if (irqs_pipelined()) {
		WARN_ON_ONCE(irq_pipeline_debug() && !in_pipeline());
		if (irqd_is_wakeup_armed(irqd))
			return true;
		if (irq_active_on_this_cpu(desc))
			return true;
	} else if (unlikely(irqd_is_wakeup_armed(irqd))) {
		irq_pm_handle_wakeup(desc);
		return false;
	}

	/* Check whether the interrupt is polled on another CPU */
	if (unlikely(desc->istate & IRQS_POLL_INPROGRESS)) {
		if (WARN_ONCE(irq_poll_cpu == smp_processor_id(),
			      "irq poll in progress on cpu %d for irq %d\n",
			      smp_processor_id(), desc->irq_data.irq))
			return false;
		return irq_wait_on_inprogress(desc);
	}

	/* The below works only for single target interrupts */
	if (!IS_ENABLED(CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK) ||
	    !irqd_is_single_target(irqd) || desc->handle_irq != handle_edge_irq)
		return false;

	/*
	 * If the interrupt affinity was moved to this CPU and the
	 * interrupt is currently handled on the previous target CPU, then
	 * busy wait for INPROGRESS to be cleared. Otherwise for edge type
	 * interrupts the handler might get stuck on the previous target:
	 *
	 * CPU 0			CPU 1 (new target)
	 * handle_edge_irq()
	 * repeat:
	 *	handle_event()		handle_edge_irq()
	 *			        if (INPROGESS) {
	 *				  set(PENDING);
	 *				  mask();
	 *				  return;
	 *				}
	 *	if (PENDING) {
	 *	  clear(PENDING);
	 *	  unmask();
	 *	  goto repeat;
	 *	}
	 *
	 * This happens when the device raises interrupts with a high rate
	 * and always before handle_event() completes and the CPU0 handler
	 * can clear INPROGRESS. This has been observed in virtual machines.
	 */
	aff = irq_data_get_effective_affinity_mask(irqd);
	if (cpumask_first(aff) != smp_processor_id())
		return false;
	return irq_wait_on_inprogress(desc);
}

static inline bool irq_can_handle_actions(struct irq_desc *desc)
{
	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		return false;
	}
	return true;
}

static inline bool irq_can_handle(struct irq_desc *desc)
{
	if (!irq_can_handle_pm(desc))
		return false;

	return irq_can_handle_actions(desc);
}

/**
 * handle_nested_irq - Handle a nested irq from a irq thread
 * @irq:	the interrupt number
 *
 * Handle interrupts which are nested into a threaded interrupt
 * handler. The handler function is called inside the calling threads
 * context.
 */
void handle_nested_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;
	irqreturn_t action_ret;

	might_sleep();

	scoped_guard(hybrid_spinlock_irq, &desc->lock) {
		if (!irq_can_handle_actions(desc))
			return;

		action = desc->action;
		kstat_incr_irqs_this_cpu(desc);
		atomic_inc(&desc->threads_active);
	}

	action_ret = IRQ_NONE;
	for_each_action_of_desc(desc, action)
		action_ret |= action->thread_fn(action->irq, action->dev_id);

	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, action_ret);

	wake_threads_waitq(desc);
}
EXPORT_SYMBOL_GPL(handle_nested_irq);

/**
 * handle_simple_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Simple interrupts are either sent from a demultiplexing interrupt
 * handler or come from hardware, where no interrupt hardware control is
 * necessary.
 *
 * Note: The caller is expected to handle the ack, clear, mask and unmask
 * issues if necessary.
 */
void handle_simple_irq(struct irq_desc *desc)
{
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	flow = get_flow_step(desc);
	if (may_start_flow(flow) && !irq_can_handle_pm(desc))
		return;

	if (should_feed_pipeline(desc, flow)) {
		handle_oob_irq(desc);
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
	} else {
		if (!irq_can_handle_actions(desc))
			return;

		kstat_incr_irqs_this_cpu(desc);
		handle_irq_event(desc);
	}
}
EXPORT_SYMBOL_GPL(handle_simple_irq);

/**
 * handle_untracked_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Untracked interrupts are sent from a demultiplexing interrupt handler
 * when the demultiplexer does not know which device it its multiplexed irq
 * domain generated the interrupt. IRQ's handled through here are not
 * subjected to stats tracking, randomness, or spurious interrupt
 * detection.
 *
 * Note: Like handle_simple_irq, the caller is expected to handle the ack,
 * clear, mask and unmask issues if necessary.
 */
void handle_untracked_irq(struct irq_desc *desc)
{
	int flow;

	scoped_guard(hybrid_spinlock, &desc->lock) {
		flow = get_flow_step(desc);
		if (may_start_flow(flow) && !irq_can_handle_pm(desc))
			return;

		if (should_feed_pipeline(desc, flow)) {
			handle_oob_irq(desc);
			return;
		}

		if (flow == IRQ_FLOW_FORWARD) {
			forward_irq_event(desc);
			return;
		}

		if (!irq_can_handle_actions(desc))
			return;

		desc->istate &= ~IRQS_PENDING;
		irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	}

	__handle_irq_event_percpu(desc);

	scoped_guard(hybrid_spinlock, &desc->lock)
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
}
EXPORT_SYMBOL_GPL(handle_untracked_irq);

/*
 * Called unconditionally from handle_level_irq() and only for oneshot
 * interrupts from handle_fasteoi_irq()
 */
static void cond_unmask_irq(struct irq_desc *desc)
{
	/*
	 * We need to unmask in the following cases:
	 * - Standard level irq (IRQF_ONESHOT is not set)
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot)
		unmask_irq(desc);
}

/**
 * handle_level_irq - Level type irq handler
 * @desc:	the interrupt description structure for this irq
 *
 * Level type interrupts are active as long as the hardware line has the
 * active level. This may require to mask the interrupt and unmask it after
 * the associated handler has acknowledged the device, so the interrupt
 * line is back to inactive.
 */
void handle_level_irq(struct irq_desc *desc)
{
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	flow = get_flow_step(desc);
	if (may_start_flow(flow)) {
		mask_ack_irq(desc);

		if (!irq_can_handle(desc))
			return;
	}

	if (should_feed_pipeline(desc, flow)) {
		if (handle_oob_irq(desc))
			goto out_unmask;
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);
	handle_irq_event(desc);

out_unmask:
	cond_unmask_irq(desc);
}
EXPORT_SYMBOL_GPL(handle_level_irq);

static void cond_unmask_eoi_irq(struct irq_desc *desc, struct irq_chip *chip)
{
	if (!(desc->istate & IRQS_ONESHOT)) {
		if (!irqs_pipelined())
			chip->irq_eoi(&desc->irq_data);
		else if (!irqd_irq_disabled(&desc->irq_data))
			unmask_irq(desc);

		return;
	}
	/*
	 * We need to unmask in the following cases:
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot) {
		if (!irqs_pipelined())
			chip->irq_eoi(&desc->irq_data);
		unmask_irq(desc);
	} else if (!irqs_pipelined() && !(chip->flags & IRQCHIP_EOI_THREADED)) {
		chip->irq_eoi(&desc->irq_data);
	}
}

static inline void cond_eoi_irq(struct irq_chip *chip, struct irq_data *data, int flags)
{
	if (!(chip->flags & flags))
		chip->irq_eoi(data);
}

static inline void mask_cond_eoi_irq(struct irq_chip *chip, struct irq_desc *desc)
{
	mask_irq(desc);
	cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_THREADED);
}

/**
 * handle_fasteoi_irq - irq handler for transparent controllers
 * @desc:	the interrupt description structure for this irq
 *
 * Only a single callback will be issued to the chip: an ->eoi() call when
 * the interrupt has been serviced. This enables support for modern forms
 * of interrupt handlers, which handle the flow details in hardware,
 * transparently.
 */
void handle_fasteoi_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	/*
	 * When an affinity change races with IRQ handling, the next interrupt
	 * can arrive on the new CPU before the original CPU has completed
	 * handling the previous one - it may need to be resent.
	 */
	flow = get_flow_step(desc);
	if (may_start_flow(flow) && !irq_can_handle_pm(desc)) {
		if (irqd_needs_resend_when_in_progress(&desc->irq_data))
			desc->istate |= IRQS_PENDING;
		cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_IF_HANDLED);
		return;
	}

	if (should_feed_pipeline(desc, flow)) {
		if (handle_oob_irq(desc))
			chip->irq_eoi(&desc->irq_data);
		else
			mask_cond_eoi_irq(chip, desc);
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
		return;

	}

	if (!irq_can_handle_actions(desc)) {
		mask_irq(desc);
		cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_IF_HANDLED);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (!irqs_pipelined() && (desc->istate & IRQS_ONESHOT))
		mask_irq(desc);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);

	/*
	 * When the race described above happens this will resend the interrupt.
	 */
	if (unlikely(desc->istate & IRQS_PENDING))
		check_irq_resend(desc, false);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_irq);

/**
 *	handle_fasteoi_nmi - irq handler for NMI interrupt lines
 *	@desc:	the interrupt description structure for this irq
 *
 *	A simple NMI-safe handler, considering the restrictions
 *	from request_nmi.
 *
 *	Only a single callback will be issued to the chip: an ->eoi()
 *	call when the interrupt has been serviced. This enables support
 *	for modern forms of interrupt handlers, which handle the flow
 *	details in hardware, transparently.
 */
void handle_fasteoi_nmi(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqaction *action = desc->action;
	unsigned int irq = irq_desc_get_irq(desc);
	irqreturn_t res;

	__kstat_incr_irqs_this_cpu(desc);

	trace_irq_handler_entry(irq, action);
	/*
	 * NMIs cannot be shared, there is only one action.
	 */
	res = action->handler(irq, action->dev_id);
	trace_irq_handler_exit(irq, action, res);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_nmi);

/**
 * handle_edge_irq - edge type IRQ handler
 * @desc:	the interrupt description structure for this irq
 *
 * Interrupt occurs on the falling and/or rising edge of a hardware
 * signal. The occurrence is latched into the irq controller hardware and
 * must be acked in order to be reenabled. After the ack another interrupt
 * can happen on the same source even before the first one is handled by
 * the associated event handler. If this happens it might be necessary to
 * disable (mask) the interrupt depending on the controller hardware. This
 * requires to reenable the interrupt inside of the loop which handles the
 * interrupts which have arrived while the handler was running. If all
 * pending interrupts are handled, the loop is left.
 */
void handle_edge_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	flow = get_flow_step(desc);
	if (may_start_flow(flow) && !irq_can_handle(desc)) {
		desc->istate |= IRQS_PENDING;
		mask_ack_irq(desc);
		return;
	}

	if (should_feed_pipeline(desc, flow)) {
		chip->irq_ack(&desc->irq_data);
		desc->istate |= IRQS_EDGE;
		handle_oob_irq(desc);
		desc->istate &= ~IRQS_EDGE;
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);

	/* Start handling the irq */
	if (!irqs_pipelined())
		chip->irq_ack(&desc->irq_data);

	do {
		if (unlikely(!desc->action)) {
			mask_irq(desc);
			return;
		}

		/*
		 * When another irq arrived while we were handling
		 * one, we could have masked the irq.
		 * Reenable it, if it was not disabled in meantime.
		 */
		if (unlikely(desc->istate & IRQS_PENDING)) {
			if (!irqd_irq_disabled(&desc->irq_data) &&
			    irqd_irq_masked(&desc->irq_data))
				unmask_irq(desc);
		}

		handle_irq_event(desc);

	} while ((desc->istate & IRQS_PENDING) && !irqd_irq_disabled(&desc->irq_data));
}
EXPORT_SYMBOL(handle_edge_irq);

/**
 *	handle_percpu_irq - Per CPU local irq handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Per CPU interrupts on SMP machines without locking requirements
 */
void handle_percpu_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long flags;
	bool handled;
	int flow;

	if (irqs_pipelined()) {
		flow = get_flow_step(desc);
		if (may_start_flow(flow)) {
			if (chip->irq_ack)
				chip->irq_ack(&desc->irq_data);
			handled = handle_oob_irq(desc);
			if (chip->irq_eoi)
				chip->irq_eoi(&desc->irq_data);
			if (!handled && chip->irq_mask)
				chip->irq_mask(&desc->irq_data);
		} else if (flow == IRQ_FLOW_FORWARD) {
			handle_irq_event_percpu(desc);
		} else {
			__kstat_incr_irqs_this_cpu(desc);
			handle_irq_event_percpu(desc);
			/*
			 * irqchip handlers assume that hard irqs are
			 * off, which is not the case on replay of
			 * unserialized percpu irqs: fix this up.
			 */
			if (chip->irq_unmask) {
				flags = hard_cond_local_irq_save();
				chip->irq_unmask(&desc->irq_data);
				hard_cond_local_irq_restore(flags);
			}
		}
		return;
	}

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);

	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	handle_irq_event_percpu(desc);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/**
 * handle_percpu_devid_irq - Per CPU local irq handler with per cpu dev ids
 * @desc:	the interrupt description structure for this irq
 *
 * Per CPU interrupts on SMP machines without locking requirements. Same as
 * handle_percpu_irq() above but with the following extras:
 *
 * action->percpu_dev_id is a pointer to percpu variables which
 * contain the real device id for the cpu on which this handler is
 * called.
 *
 * May be used for NMI interrupt lines, and so may be called in IRQ or NMI
 * context.
 */
void handle_percpu_devid_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int irq = irq_desc_get_irq(desc);
	unsigned int cpu = smp_processor_id();
	struct irqaction *action;
	unsigned long flags;
	irqreturn_t res;
	bool handled;
	int flow;

	flow = get_flow_step(desc);
	if (irqs_pipelined()) {
		if (may_start_flow(flow)) {
			if (chip->irq_ack)
				chip->irq_ack(&desc->irq_data);
			handled = handle_oob_irq(desc);
			if (chip->irq_eoi)
				chip->irq_eoi(&desc->irq_data);
			if (!handled && chip->irq_mask)
				chip->irq_mask(&desc->irq_data);
			return;
		} else if (flow == IRQ_FLOW_FORWARD) {
			handle_irq_event_percpu(desc);
			return;
		}
	} else if (chip->irq_ack) {
		chip->irq_ack(&desc->irq_data);
	}

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);

	for (action = desc->action; action; action = action->next)
		if (cpumask_test_cpu(cpu, action->affinity))
			break;

	if (likely(action)) {
		trace_irq_handler_entry(irq, action);
		res = action->handler(irq, raw_cpu_ptr(action->percpu_dev_id));
		trace_irq_handler_exit(irq, action, res);
	} else {
		bool enabled = cpumask_test_cpu(cpu, desc->percpu_enabled);

		if (enabled)
			irq_percpu_disable(desc, cpu);

		pr_err_once("Spurious%s percpu IRQ%u on CPU%u\n",
			    enabled ? " and unmasked" : "", irq, cpu);
	}

	if (irqs_pipelined()) {
		/*
		 * irqchip handlers assume that hard irqs are off,
		 * which is not the case on replay of unserialized
		 * percpu irqs: fix this up.
		 */
		flags = hard_cond_local_irq_save();
		if (chip->irq_unmask)
			chip->irq_unmask(&desc->irq_data);
		hard_cond_local_irq_restore(flags);
	} else if (chip->irq_eoi) {
		chip->irq_eoi(&desc->irq_data);
	}
}

static void
__irq_do_set_handler(struct irq_desc *desc, irq_flow_handler_t handle,
		     int is_chained, const char *name)
{
	if (!handle) {
		handle = handle_bad_irq;
	} else {
		struct irq_data *irq_data = &desc->irq_data;
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		/*
		 * With hierarchical domains we might run into a
		 * situation where the outermost chip is not yet set
		 * up, but the inner chips are there.  Instead of
		 * bailing we install the handler, but obviously we
		 * cannot enable/startup the interrupt at this point.
		 */
		while (irq_data) {
			if (irq_data->chip != &no_irq_chip)
				break;
			/*
			 * Bail out if the outer chip is not set up
			 * and the interrupt supposed to be started
			 * right away.
			 */
			if (WARN_ON(is_chained))
				return;
			/* Try the parent */
			irq_data = irq_data->parent_data;
		}
#endif
		if (WARN_ON(!irq_data || irq_data->chip == &no_irq_chip))
			return;
	}

	/* Uninstall? */
	if (handle == handle_bad_irq) {
		if (desc->irq_data.chip != &no_irq_chip)
			mask_ack_irq(desc);
		irq_state_set_disabled(desc);
		if (is_chained) {
			desc->action = NULL;
			irq_chip_pm_put(irq_desc_get_irq_data(desc));
		}
		desc->depth = 1;
	}
	desc->handle_irq = handle;
	desc->name = name;

	if (handle != handle_bad_irq && is_chained) {
		unsigned int type = irqd_get_trigger_type(&desc->irq_data);

		/*
		 * We're about to start this interrupt immediately,
		 * hence the need to set the trigger configuration.
		 * But the .set_type callback may have overridden the
		 * flow handler, ignoring that we're dealing with a
		 * chained interrupt. Reset it immediately because we
		 * do know better.
		 */
		if (type != IRQ_TYPE_NONE) {
			__irq_set_trigger(desc, type);
			desc->handle_irq = handle;
		}

		irq_settings_set_noprobe(desc);
		irq_settings_set_norequest(desc);
		irq_settings_set_nothread(desc);
		desc->action = &chained_action;
		WARN_ON(irq_chip_pm_get(irq_desc_get_irq_data(desc)));
		irq_activate_and_startup(desc, IRQ_RESEND);
	}
	irq_proc_update_valid(desc);
}

void __irq_set_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		       const char *name)
{
	scoped_irqdesc_get_and_buslock(irq, 0)
		__irq_do_set_handler(scoped_irqdesc, handle, is_chained, name);
}
EXPORT_SYMBOL_GPL(__irq_set_handler);

void irq_set_chained_handler_and_data(unsigned int irq, irq_flow_handler_t handle,
				      void *data)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;

		desc->irq_common_data.handler_data = data;
		__irq_do_set_handler(desc, handle, 1, NULL);
	}
}
EXPORT_SYMBOL_GPL(irq_set_chained_handler_and_data);

void
irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name)
{
	irq_set_chip(irq, chip);
	__irq_set_handler(irq, handle, 0, name);
}
EXPORT_SYMBOL_GPL(irq_set_chip_and_handler_name);

void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;
		unsigned long trigger, tmp;
		/*
		 * Warn when a driver sets the no autoenable flag on an already
		 * active interrupt.
		 */
		WARN_ON_ONCE(!desc->depth && (set & _IRQ_NOAUTOEN));

		irq_settings_clr_and_set(desc, clr, set);

		trigger = irqd_get_trigger_type(&desc->irq_data);

		irqd_clear(&desc->irq_data, IRQD_NO_BALANCING | IRQD_PER_CPU |
			   IRQD_TRIGGER_MASK | IRQD_LEVEL);
		if (irq_settings_has_no_balance_set(desc))
			irqd_set(&desc->irq_data, IRQD_NO_BALANCING);
		if (irq_settings_is_per_cpu(desc))
			irqd_set(&desc->irq_data, IRQD_PER_CPU);
		if (irq_settings_is_level(desc))
			irqd_set(&desc->irq_data, IRQD_LEVEL);

		tmp = irq_settings_get_trigger_mask(desc);
		if (tmp != IRQ_TYPE_NONE)
			trigger = tmp;

		irqd_set(&desc->irq_data, trigger);
		irq_proc_update_valid(desc);
	}
}
EXPORT_SYMBOL_GPL(irq_modify_status);

#ifdef CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE
/**
 *	irq_cpu_online - Invoke all irq_cpu_online functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_online()
 *	for each.
 */
void irq_cpu_online(void)
{
	unsigned int irq;

	for_each_active_irq(irq) {
		struct irq_desc *desc = irq_to_desc(irq);
		struct irq_chip *chip;

		if (!desc)
			continue;

		guard(hybrid_spinlock_irqsave)(&desc->lock);
		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_online &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_online(&desc->irq_data);
	}
}

/**
 *	irq_cpu_offline - Invoke all irq_cpu_offline functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_offline()
 *	for each.
 */
void irq_cpu_offline(void)
{
	unsigned int irq;

	for_each_active_irq(irq) {
		struct irq_desc *desc = irq_to_desc(irq);
		struct irq_chip *chip;

		if (!desc)
			continue;

		guard(hybrid_spinlock_irqsave)(&desc->lock);
		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_offline &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_offline(&desc->irq_data);
	}
}
#endif

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY

#ifdef CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS
/**
 * handle_fasteoi_ack_irq - irq handler for edge hierarchy stacked on
 *			    transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_ack() function called.
 */
void handle_fasteoi_ack_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	flow = get_flow_step(desc);
	if (may_start_flow(flow) && !irq_can_handle_pm(desc)) {
		cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_IF_HANDLED);
		return;
	}

	if (should_feed_pipeline(desc, flow)) {
		chip->irq_ack(&desc->irq_data);
		if (handle_oob_irq(desc))
			chip->irq_eoi(&desc->irq_data);
		else
			mask_cond_eoi_irq(chip, desc);
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
		return;
	}

	if (unlikely(!irq_can_handle_actions(desc))) {
		mask_irq(desc);
		cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_IF_HANDLED);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (!irqs_pipelined()) {
		if (desc->istate & IRQS_ONESHOT)
			mask_irq(desc);

		chip->irq_ack(&desc->irq_data);
	}

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_ack_irq);

/**
 * handle_fasteoi_mask_irq - irq handler for level hierarchy stacked on
 *			     transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_mask_ack() function called.
 */
void handle_fasteoi_mask_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;
	int flow;

	guard(hybrid_spinlock)(&desc->lock);

	flow = get_flow_step(desc);
	if (may_start_flow(flow)) {
		mask_ack_irq(desc);

		if (!irq_can_handle_pm(desc)) {
			cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_IF_HANDLED);
			return;
		}
	}

	if (should_feed_pipeline(desc, flow)) {
		if (handle_oob_irq(desc))
			cond_unmask_eoi_irq(desc, chip);
		else
			cond_eoi_irq(chip, &desc->irq_data, IRQCHIP_EOI_THREADED);
		return;
	}

	if (flow == IRQ_FLOW_FORWARD) {
		forward_irq_event(desc);
		return;
	}

	if (!irq_can_handle_actions(desc)) {
		mask_irq(desc);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_mask_irq);

#endif /* CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS */

#ifdef CONFIG_SMP
void irq_chip_pre_redirect_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_pre_redirect(data);
}
EXPORT_SYMBOL_GPL(irq_chip_pre_redirect_parent);
#endif

/**
 * irq_chip_set_parent_state - set the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: State to be restored (one of IRQCHIP_STATE_*)
 * @val: Value corresponding to @which
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
int irq_chip_set_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool val)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_set_irqchip_state)
		return 0;

	return data->chip->irq_set_irqchip_state(data, which, val);
}
EXPORT_SYMBOL_GPL(irq_chip_set_parent_state);

/**
 * irq_chip_get_parent_state - get the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: one of IRQCHIP_STATE_* the caller wants to know
 * @state: a pointer to a boolean where the state is to be stored
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
int irq_chip_get_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool *state)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_get_irqchip_state)
		return 0;

	return data->chip->irq_get_irqchip_state(data, which, state);
}
EXPORT_SYMBOL_GPL(irq_chip_get_parent_state);

/**
 * irq_chip_shutdown_parent - Shutdown the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_shutdown() callback of the parent if available or falls
 * back to irq_chip_disable_parent().
 */
void irq_chip_shutdown_parent(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent->chip->irq_shutdown)
		parent->chip->irq_shutdown(parent);
	else
		irq_chip_disable_parent(data);
}
EXPORT_SYMBOL_GPL(irq_chip_shutdown_parent);

/**
 * irq_chip_startup_parent - Startup the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_startup() callback of the parent if available or falls
 * back to irq_chip_enable_parent().
 */
unsigned int irq_chip_startup_parent(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent->chip->irq_startup)
		return parent->chip->irq_startup(parent);

	irq_chip_enable_parent(data);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_startup_parent);

/**
 * irq_chip_enable_parent - Enable the parent interrupt (defaults to unmask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_enable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_enable)
		data->chip->irq_enable(data);
	else
		data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_enable_parent);

/**
 * irq_chip_disable_parent - Disable the parent interrupt (defaults to mask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_disable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_disable)
		data->chip->irq_disable(data);
	else
		data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_disable_parent);

/**
 * irq_chip_ack_parent - Acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_ack_parent);

/**
 * irq_chip_mask_parent - Mask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_mask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_parent);

/**
 * irq_chip_mask_ack_parent - Mask and acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_mask_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_ack_parent);

/**
 * irq_chip_unmask_parent - Unmask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_unmask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_unmask_parent);

/**
 * irq_chip_eoi_parent - Invoke EOI on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_eoi_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_eoi(data);
}
EXPORT_SYMBOL_GPL(irq_chip_eoi_parent);

/**
 * irq_chip_set_affinity_parent - Set affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @dest:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_affinity_parent(struct irq_data *data,
				 const struct cpumask *dest, bool force)
{
	data = data->parent_data;
	if (data->chip->irq_set_affinity)
		return data->chip->irq_set_affinity(data, dest, force);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_affinity_parent);

/**
 * irq_chip_set_type_parent - Set IRQ type on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_type_parent(struct irq_data *data, unsigned int type)
{
	data = data->parent_data;

	if (data->chip->irq_set_type)
		return data->chip->irq_set_type(data, type);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_type_parent);

/**
 * irq_chip_retrigger_hierarchy - Retrigger an interrupt in hardware
 * @data:	Pointer to interrupt specific data
 *
 * Iterate through the domain hierarchy of the interrupt and check
 * whether a hw retrigger function exists. If yes, invoke it.
 */
int irq_chip_retrigger_hierarchy(struct irq_data *data)
{
	for (data = data->parent_data; data; data = data->parent_data)
		if (data->chip && data->chip->irq_retrigger)
			return data->chip->irq_retrigger(data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_retrigger_hierarchy);

/**
 * irq_chip_set_vcpu_affinity_parent - Set vcpu affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @vcpu_info:	The vcpu affinity information
 */
int irq_chip_set_vcpu_affinity_parent(struct irq_data *data, void *vcpu_info)
{
	data = data->parent_data;
	if (data->chip->irq_set_vcpu_affinity)
		return data->chip->irq_set_vcpu_affinity(data, vcpu_info);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_vcpu_affinity_parent);
/**
 * irq_chip_set_wake_parent - Set/reset wake-up on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @on:		Whether to set or reset the wake-up capability of this irq
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_wake_parent(struct irq_data *data, unsigned int on)
{
	data = data->parent_data;

	if (data->chip->flags & IRQCHIP_SKIP_SET_WAKE)
		return 0;

	if (data->chip->irq_set_wake)
		return data->chip->irq_set_wake(data, on);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_wake_parent);

/**
 * irq_chip_request_resources_parent - Request resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
int irq_chip_request_resources_parent(struct irq_data *data)
{
	data = data->parent_data;

	if (data->chip->irq_request_resources)
		return data->chip->irq_request_resources(data);

	/* no error on missing optional irq_chip::irq_request_resources */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_request_resources_parent);

/**
 * irq_chip_release_resources_parent - Release resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_release_resources_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_release_resources)
		data->chip->irq_release_resources(data);
}
EXPORT_SYMBOL_GPL(irq_chip_release_resources_parent);
#endif /* CONFIG_IRQ_DOMAIN_HIERARCHY */

#ifdef CONFIG_SMP
int irq_chip_redirect_set_affinity(struct irq_data *data, const struct cpumask *dest, bool force)
{
	struct irq_redirect *redir = &irq_data_to_desc(data)->redirect;

	WRITE_ONCE(redir->target_cpu, cpumask_first(dest));
	irq_data_update_effective_affinity(data, dest);

	return IRQ_SET_MASK_OK_DONE;
}
EXPORT_SYMBOL_GPL(irq_chip_redirect_set_affinity);
#endif

/**
 * irq_chip_compose_msi_msg - Compose msi message for a irq chip
 * @data:	Pointer to interrupt specific data
 * @msg:	Pointer to the MSI message
 *
 * For hierarchical domains we find the first chip in the hierarchy
 * which implements the irq_compose_msi_msg callback. For non
 * hierarchical we use the top level chip.
 */
int irq_chip_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_data *pos;

	for (pos = NULL; !pos && data; data = irqd_get_parent_data(data)) {
		if (data->chip && data->chip->irq_compose_msi_msg)
			pos = data;
	}

	if (!pos)
		return -ENOSYS;

	pos->chip->irq_compose_msi_msg(pos, msg);
	return 0;
}

static struct device *irq_get_pm_device(struct irq_data *data)
{
	if (data->domain)
		return data->domain->pm_dev;

	return NULL;
}

/**
 * irq_chip_pm_get - Enable power for an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Enable the power to the IRQ chip referenced by the interrupt data
 * structure.
 */
int irq_chip_pm_get(struct irq_data *data)
{
	struct device *dev = irq_get_pm_device(data);
	int retval = 0;

	if (IS_ENABLED(CONFIG_PM) && dev)
		retval = pm_runtime_resume_and_get(dev);

	return retval;
}

/**
 * irq_chip_pm_put - Drop a PM reference on an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Drop a power management reference, acquired via irq_chip_pm_get(), on the IRQ
 * chip represented by the interrupt data structure.
 *
 * Note that this will not disable power to the IRQ chip until this function
 * has been called for all IRQs that have called irq_chip_pm_get() and it may
 * not disable power at all (if user space prevents that, for example).
 */
void irq_chip_pm_put(struct irq_data *data)
{
	struct device *dev = irq_get_pm_device(data);

	if (dev)
		pm_runtime_put(dev);
}
