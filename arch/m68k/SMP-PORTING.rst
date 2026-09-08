.. SPDX-License-Identifier: GPL-2.0

===========================
Porting SMP to an m68k board
===========================

This branch adds symmetric multi-processing to Linux/m68k.  It is the SMP delta
extracted from a working dual-68040 port (the ELTEC EUROCOM-17); the e17 code is
kept as the *reference implementation* of the board-specific glue.  This document
tells you which parts are generic (reuse as-is) and which parts you must
reimplement for your machine.

.. note::

   This branch is a delta on a clean mainline base; it is **not** meant to build
   standalone.  It carries the generic m68k SMP core plus one board example
   (``arch/m68k/eltec/smp_e17.c``, which references its board's ``config.c`` /
   device tree that are *not* on this branch).  Integrate the generic pieces into
   your own board port and write your own equivalent of ``smp_e17.c``.


What SMP on m68k actually requires
==================================

m68k has no interrupt-controller abstraction shared across platforms and no
existing SMP machine, so "SMP support" is three separable things:

1. **Generic arch plumbing** -- SMP-safe atomics/bitops/barriers, a real
   cross-CPU futex, TLB shootdown, per-CPU sections, spinlocks.  Reuse verbatim.

2. **The generic SMP core** -- ``kernel/smp.c`` (cpu-up, the IPI dispatch, the
   cross-call registration) and the ``smp_secondary.S`` trampoline skeleton.
   Reuse, but note the core currently calls the e17 board hooks directly (see
   `The board interface contract`_); factor those to your board.

3. **Board bring-up glue** -- release the secondary from reset, the IPI
   doorbells, a per-CPU tick, and cache/bus coherency.  Rewrite for your machine
   using ``arch/m68k/eltec/smp_e17.c`` + ``asm/smp_e17.h`` as the template.


Generic layer -- reuse as-is
============================

These are board-independent and should need no change:

``arch/m68k/include/asm/atomic.h``, ``bitops.h``, ``barrier.h``
  SMP-safe via the 680x0 ``cas`` instruction (``select RMW_INSNS``) and real
  memory barriers.  On UP these compile down to the old irq-disable versions.

``arch/m68k/include/asm/tlbflush.h``
  TLB shootdown across CPUs (the flush is broadcast; on 040/060 there is no
  remote-invalidate instruction, so it goes through an IPI -- see ``smp.c``).

``arch/m68k/include/asm/futex.h`` + ``arch/m68k/kernel/futex.c``
  **Read this if nothing else.**  The 68040 ``cas`` can only address memory
  through the supervisor root pointer (no ``moves``/alternate-address-space
  form), and Linux only switches *URP* per process -- so a supervisor ``cas`` on
  a user virtual address never reaches the user page.  A kernel lock does not
  help either, because m68k userspace does its own ``cas`` on the futex word.
  ``futex.c`` therefore resolves the user address by walking ``current->mm`` and
  does the ``cas`` on the page's *kernel linear-map alias*; the physically-tagged
  D-cache + the ``cas`` bus lock make that atomic against a concurrent user-mode
  ``cas`` on the same physical word.  Without this, multithreaded userspace gets
  lost wakeups / corrupted futex words under SMP.  This logic is generic to any
  680x0 SMP machine.

``arch/m68k/include/asm/Kbuild``
  Switches ``spinlock.h`` / ``qrwlock`` to the asm-generic queued locks
  (``select ARCH_USE_QUEUED_RWLOCKS``).

``arch/m68k/kernel/vmlinux-std.lds``
  Adds ``PERCPU_SECTION(16)`` in the init-freed region (defines
  ``__per_cpu_start/end`` for ``mm/percpu.c``).  If your board uses a different
  linker script (``vmlinux-nommu.lds`` etc.) add the same there.


The generic SMP core
====================

``arch/m68k/kernel/smp.c``
  cpu bring-up (``__cpu_up``), ``smp_prepare_cpus``, ``secondary_start_kernel``,
  and IPI dispatch for ``IPI_RESCHEDULE`` / ``IPI_CALL_FUNC`` /
  ``IPI_CALL_FUNC_SINGLE`` / ``IPI_CPU_STOP``.  The send side is a function
  pointer registered by the board with ``set_smp_cross_call()``.  A software
  pending bitmap (``e17_ipi_set_pending`` / ``_take_pending``) is the poll-only
  baseline used before real doorbell IRQs exist.

``arch/m68k/include/asm/smp.h``
  The IPI message set and per-CPU declarations.

``arch/m68k/kernel/smp_secondary.S``
  The secondary trampoline: the AP comes out of release running **physical, MMU
  off**, sets up a transparent-translation window, loads its stack + idle task
  from a descriptor the boot CPU planted, enables the MMU, and jumps to
  ``secondary_start_kernel``.  The skeleton is generic; the entry address, the
  descriptor layout and any cache/snoop enable are board-specific.

``arch/m68k/mm/cache.c``, ``arch/m68k/kernel/process.c``
  SMP cache maintenance and the AP idle loop.

.. warning::

   As shipped, ``smp.c`` and ``smp_secondary.S`` call the e17 hooks directly
   (``e17_boot_secondary``, ``e17_park_secondary``, ``e17_smp_enable_snoop``,
   ``E17_CPU2CON`` ...) and ``#include <asm/smp_e17.h>``.  When you add a second
   board, lift these to a small ``struct m68k_smp_ops`` (or weak functions) and
   provide your board's implementation.  They are called out inline so they are
   easy to find (``grep -n e17 arch/m68k/kernel/smp.c``).


The board interface contract
============================

Everything the generic core needs from your platform.  ``smp_e17.c`` /
``asm/smp_e17.h`` implement all of these for the EUROCOM-17.

Secondary release -- ``boot_secondary(cpu, idle)``
  Plant the trampoline descriptor (physical entry, kernel stack, idle task,
  page-table root) and pulse the secondary out of reset.  On the e17 this is the
  CPU2CON ``SRESET`` bit at ``$FEC58000``.  Return once the AP has checked in.

IPI send -- register with ``set_smp_cross_call(fn)``
  ``fn(cpumask, msg)`` rings a hardware doorbell on each target CPU.  e17:
  primary->secondary uses the **CPU2CON mailbox** (an autovectored level-6
  interrupt on the secondary); secondary->primary uses the **VIC068A ICMS**
  module-switch doorbell.  Until you have real doorbell IRQs, fall back to the
  pending-bitmap polled from the tick/idle loop (``CONFIG_E17_SMP_HW_IPI=n`` is
  the e17 example of that switch).

IPI receive
  Your doorbell ISR de-asserts the doorbell and calls the generic handler with
  the pending message(s).

Per-CPU tick
  Each CPU needs a periodic tick for RCU quiescent-state reporting and the
  scheduler tick, or the AP wedges during cpuhp bring-up.  e17 gives the AP the
  VIC "VIC-clock" as a level-4 autovector (``e17_ap_tick`` in ``config.c``).
  See `Gotchas`_ for the storm hazard.

Cache / bus coherency
  Cross-CPU atomics (``cas`` takes the bus lock) and DMA both need coherency.
  e17 either runs the 040 D-cache **write-through with hardware bus snooping**
  (SNCR at ``$FEC5E000``) or, safely, D-cache-off.  Pick one before you trust
  SMP.

CPU enumeration
  Mark the possible CPUs (``smp_init_cpus`` / the DT ``cpu@1`` node).


Bring-up order (what a boot looks like)
=======================================

1. Board init calls ``set_smp_cross_call()`` and marks CPUs possible.
2. ``smp_prepare_cpus`` sets up per-CPU areas.
3. cpuhp calls ``__cpu_up(cpu, idle)`` -> ``boot_secondary()`` plants the
   descriptor and releases the AP from reset.
4. The AP runs ``smp_secondary.S`` (physical, MMU off) -> enables MMU -> jumps to
   ``secondary_start_kernel`` -> reports online -> enables its per-CPU tick and
   snooping.
5. The boot CPU sees it online and proceeds.

Boot with ``maxcpus=1`` first: you reach a shell UP with a working console, then
``echo 1 > /sys/devices/system/cpu/cpu1/online`` to bring up the AP with dmesg
available.  ``CONFIG_HOTPLUG_CPU`` (and ``HOTPLUG_CORE_SYNC_DEAD``) make that and
offline work.


Gotchas
=======

These cost real debugging time on the e17; check them early on your board.

**68040 cas is SRP-only.**  See ``futex.c`` above.  Any place you need an atomic
on a *user* address from the kernel has the same problem.

**Do not select GENERIC_IRQ_MULTI_HANDLER.**  m68k dispatches through the per-CPU
680x0 vector table (each CPU has its own VBR); there is no single root
``handle_arch_irq``.  Selecting it sends the first timer tick to a NULL handler
and hangs the boot.  Per-CPU IRQ delivery already works via the vector table.

**A self-vectored (daisy-chained) interrupt with nothing posted hangs the whole
bus.**  If a source asserts an interrupt and then withdraws it before the CPU's
vectored IACK reaches it, the IACK gets no DTACK and the 68040 stalls mid
bus-cycle -- freezing *both* CPUs until a watchdog resets the board, with a
perfectly clean ``irq_err_count``.  Prefer controller-vectored lines (the
controller always supplies a vector and DTACKs); for any genuinely self-vectored
line make sure every enabled source stays asserted through its IACK, and never
leave an unhandled enable armed (firmware may have left some set).

**A level-held periodic tick re-fires (storms).**  If the per-CPU tick source is
a level (e.g. a square wave) delivered level-sensitively, the CPU re-enters the
tick many times per period, which can starve that CPU's ``rcu_sched`` kthread ->
RCU stall -> reset.  Deliver the tick edge-triggered / one-shot, or guard
against the storm.  (The e17 boot CPU takes the same VIC timer *edge*-triggered
and is clean; the AP takes it via a level-like path and needs a guard.)

**IPIs must not depend on the tick.**  If you ever disable a CPU's tick (e.g. a
storm guard), IPIs to it must still be delivered -- use a real interrupt
doorbell, not a tick-polled bitmap, for anything that must work tickless.

**Coherency is not optional.**  Turn on bus snooping (or D-cache-off) before
trusting cross-CPU atomics or DMA; a physically-tagged D-cache without snooping
silently corrupts shared data.


File map
========

Generic (reuse)::

  arch/m68k/include/asm/{atomic,bitops,barrier,futex,tlbflush,Kbuild}.h
  arch/m68k/include/asm/smp.h
  arch/m68k/kernel/{smp.c,smp_secondary.S,futex.c}
  arch/m68k/kernel/vmlinux-std.lds        (PERCPU_SECTION)
  arch/m68k/mm/cache.c                     (SMP maintenance)
  arch/m68k/kernel/process.c               (AP idle)
  arch/m68k/Kconfig                        (SMP / NR_CPUS / HOTPLUG_CPU)

Board reference (rewrite for your machine)::

  arch/m68k/eltec/smp_e17.c                (release, IPI doorbells, snoop, tests)
  arch/m68k/include/asm/smp_e17.h          (board register defs)
  arch/m68k/eltec/config.c                 (per-CPU tick e17_ap_tick*, enumeration)
                                           -- on the full e17 port, not this branch
