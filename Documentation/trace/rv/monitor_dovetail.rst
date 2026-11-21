.. SPDX-License-Identifier: GPL-2.0

Dovetail monitors
=================

- Name: dovetail
- Type: container for multiple monitors
- Author: Tobias Schaffner <tobias.schaffner@siemens.com>

Description
-----------

Dovetail runs a task either on the in-band stage, scheduled by Linux, or on
the out-of-band stage, scheduled by a companion core. The monitors in this
container check the contract between Dovetail and the companion core. They
are per-task monitors fed by Dovetail and generic tracepoints only, meant for
validation: while enabled, they add work to out-of-band hot paths.
``CONFIG_RV_DOVETAIL`` enables them with RV and the printk reactor.

Each monitor takes one of ``CONFIG_RV_PER_TASK_MONITORS`` slots while
enabled, so the container needs at least three, the default with
``CONFIG_RV_DOVETAIL``.

The Dovetail tracepoints fire in the context given here, so their probes must
be safe there:

==========================  ======  ============================================
Tracepoint                  Stage   Context
==========================  ======  ============================================
dovetail_leave_inband_tp    oob     after a successful switch; oob stage
                                    stalled, hard IRQs off, the core may hold
                                    its runqueue lock
dovetail_leave_oob_tp       oob     before the switch to in-band, in the core
dovetail_trap_notify_tp     oob     hard IRQs off, before the core is notified
dovetail_trap_unwind_tp     either  hard IRQs off, before the core is notified
dovetail_oob_return_tp      oob     on return to user mode from an interrupt,
                                    an oob syscall or a RETUSER switch; hard
                                    IRQs on at the oob syscall return
dovetail_signal_notify_tp   inband  after the core handled the notification,
                                    siglock held
dovetail_irqoff_entry_tp    either  hard IRQs off, from the generic entry code,
dovetail_irqoff_exit_tp             possibly in NMI context
==========================  ======  ============================================

Monitor offstage
++++++++++++++++

Linux leaves a task alone while it runs out-of-band: it neither schedules
it nor runs signal code for it, as a trap handler raising a signal before
the companion core demoted the task would::

                 sched_in
                 signal_self    leave_inband
                  +--+  ----------------> +-----+
                  |  v                    |     |
                 +--------+               | oob |
                 | inband |               |     |
                 +--------+ <------------ +-----+
                              leave_oob

``sched_in`` is a ``sched_switch`` to the task, ``signal_self`` a
``signal_generate`` by the task for itself.

Monitor oob_trap
++++++++++++++++

The Dovetail trap contract, per task:

- an exception the arch code must notify, taken out-of-band, is notified
  next, with the trap number of the exception: the arch code reports it
  (``trap_entry``, from ``dovetail_trap_entry()``) where it tells exceptions
  apart, before handling it, so a handler which does not notify, or notifies
  the wrong trap number (``right_trap``), is reported. The arch code reports
  the exception classes it must always notify: x86 user mode exceptions,
  breakpoints and page faults on user addresses, arm64 aborts on user
  addresses, alignment, undefined instruction, BTI, FP, SVE/SME and debug
  exceptions, every riscv exception but syscalls. A nested exception taken
  out-of-band while a notified trap is handled (e.g. a faulting kprobe fetch
  in a kernel trap the core did not demote for) is not notified again;
- every out-of-band trap notification is unwound once, for the same trap and
  by the same handler level: the trap key folds in the stack frame of the
  arch code issuing the notify and the unwind (``oob_trap_notify()`` and
  ``oob_trap_unwind()`` are inlined into it), so a nested handler unwinding
  the trap of its caller is reported;
- the core's trap exit handler returns with hard irqs off (``trap_exit_*``,
  traced right after ``handle_oob_trap_exit()``). The core may demote the
  task for the trap (``leave_oob`` while trapped) and promote it again after
  the unwind; a second demotion, or a promotion before the unwind, are
  reported. With EVL (``same_stage``), a kernel mode trap must end on the
  stage it was notified from: if EVL demoted the task for it, EVL must
  promote it again. Other cores, e.g. Cobalt, may end it in-band. With EVL
  (``demotes_user``), the task must also be demoted for every user mode trap;
- a trap ends within ``timeout_ms`` (module parameter, default 1000, 0 turns
  the check off), which catches a trap handler that hangs;
- a task does not return to user mode out-of-band (``oob_return``) with a
  notification open;
- no task is demoted from interrupt or NMI context (``leave_oob`` while on
  the pipeline entry or in NMI), e.g. by a trap hit in an out-of-band
  interrupt handler;
- no interrupt is taken in NMI context (``irq_entry``, traced on pipeline
  entry), which means hard irqs were enabled there, e.g. by a fault handler
  nested in a kernel trap the core did not demote for;
- a kernel mode exception or NMI taken over a context with hard irqs off
  keeps them off until it returns, in-band or out-of-band (``irqs_kept``): no
  interrupt is taken and no task is scheduled while it runs, e.g. after a
  page fault handler enabled hard irqs regardless of the interrupted
  context. ``dovetail_irqoff_entry_tp`` and ``dovetail_irqoff_exit_tp``,
  emitted by the generic ``irqentry_enter()``/``irqentry_exit()`` and
  ``irqentry_nmi_enter()``/``irqentry_nmi_exit()``, tell which entry is
  open on a CPU: the outermost one whose saved registers have hard irqs off
  (``regs_irqs_disabled()``). Any task taking the interrupt or scheduling
  then is checked, not only the tasks seen trapping. An interrupt has to
  arrive while the handler runs for a violation to show. Arches which do
  not use these helpers for kernel mode exceptions (arm64 outside of NMIs)
  are not covered;
- a monitored task does not schedule out while a kprobe is being handled on
  its CPU, which would let another task reenter the kprobe::

        idle --trap_entry; reset(clk)--> entered
        entered --trap_notify [right_trap]; reset(same_trap), reset(clk)--> trapped
        idle --trap_notify; reset(same_trap), reset(clk)--> trapped
        trapped, unwinding --trap_entry--> (same state)   (nested)
        trapped --leave_oob [task_ctx]--> demoted            (core demotes)
        trapped --trap_unwind [same_trap]--> unwinding       (core declined)
        demoted --trap_unwind [same_trap]--> unwinding_demoted
        unwinding_demoted --leave_inband--> unwinding        (core promotes)
        unwinding --trap_exit_kernel [irqs_off]--> idle
        unwinding --trap_exit_user [irqs_off, !demotes_user]--> idle
        unwinding_demoted --trap_exit_user [irqs_off]--> idle
        unwinding_demoted --trap_exit_kernel [irqs_off, !same_stage]--> idle

  in every state:  sched_switch [no_kprobe, irqs_kept],
                   irq_entry [not_nmi, irqs_kept]
  idle:            leave_inband, oob_return, leave_oob [task_ctx]
  other states:    clk < timeout_ns
  entered:         nothing else (the exception was not notified)

A leaked notification is seen when the task goes out-of-band again or
returns to user mode out-of-band. Traps notified in NMI context (riscv kernel
mode traps) are monitored like any other. Only tasks seen trapping or
switching stage are checked on ``sched_switch`` and ``irq_entry``. Arming a
timer is not safe out-of-band or in NMI, so the monitor arms none: it checks
the clock invariant on the task's next event (``irq_entry`` ticks while the
trap runs) with the NMI safe ``ktime_get_mono_fast_ns()``. A trap blocked in
the kernel is reported only once the task runs again.

Monitor oob_signal
++++++++++++++++++

Signals are delivered in-band, so the companion core has to kick a task
signaled out-of-band, with a mayday if the task may return to user mode
there. Once the core handled the notification, the task must not return to
user mode out-of-band without a mayday pending. A task reaching the
out-of-band stage with a signal pending is held to the same rule::

    state     leave_   leave_  oob_    oob_return_  signal
              inband   oob     return  mayday
    inband    oob      -       -       -            inband
    oob       -        inband  oob     oob          signaled
    signaled  -        inband  -       signaled     signaled

The monitor takes no lock. The core sets ``TIF_MAYDAY`` in its
``INBAND_TASK_SIGNAL`` handler, before the notification moves the task to
signaled, and a return reads ``TIF_MAYDAY`` only once it sees signaled. As the
notifier holds ``siglock``, the task cannot consume the signal and come back
out-of-band before the notification is seen. A return from a trap the core
does not demote is not seen. The monitor is not available on 32-bit arm,
whose out-of-band syscall fast path is not instrumented.

Usage
-----

::

  # cd /sys/kernel/tracing/rv
  # echo printk > monitors/dovetail/reactors
  # echo 1 > monitors/dovetail/enable
  # cat enabled_monitors
  dovetail
  dovetail:offstage
  dovetail:oob_trap
  dovetail:oob_signal

Violations are reported by the selected reactor and as ``error_*`` events in
``events/rv``. The models are in ``tools/verification/models/``.
