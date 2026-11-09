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

Every out-of-band trap notification is unwound for the same trap before the
task goes out-of-band again. A handler missing the unwind leaves
``_TLF_OOBTRAP`` set, and the companion core is no longer told about the
traps of that task::

                  trap_notify; reset(same_trap)
          +-------+   ------------------------>   +---------+
          | idle  |                               | trapped |
          +-------+   <------------------------   +---------+
           |     ^    trap_unwind; same_trap == 1
           +-----+
         leave_inband

A trap is identified by its number and register frame, so an unwind with the
wrong number, and the unwind of a trap nested in the in-band handling of
another one, are reported too. A leak is only seen once the task goes
out-of-band again. Traps taken in NMI context are ignored. The model uses no
clock, so the hybrid automaton core arms no timer and is safe on the
out-of-band stage.

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

Violations are reported by the selected reactor and as ``error_*`` events in
``events/rv``. The models are in ``tools/verification/models/``.
