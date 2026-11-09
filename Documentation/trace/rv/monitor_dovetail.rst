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

Usage
-----

::

  # cd /sys/kernel/tracing/rv
  # echo printk > monitors/dovetail/reactors
  # echo 1 > monitors/dovetail/enable
  # cat enabled_monitors
  dovetail
  dovetail:offstage

Violations are reported by the selected reactor and as ``error_*`` events in
``events/rv``. The models are in ``tools/verification/models/``.
