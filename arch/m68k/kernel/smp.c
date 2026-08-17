// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/m68k/kernel/smp.c
 *
 * Generic Linux/m68k SMP core. Board-independent: the IPI hardware and the
 * secondary release are provided by the platform through set_smp_cross_call()
 * and __cpu_up()/e17_boot_secondary() (arch/m68k/eltec/smp_e17.c).
 *
 * Structure mirrors arch/openrisc/kernel/smp.c (the cleanest in-tree example),
 * adapted to the 68040: 'current' is register %a2 and per-CPU id comes from the
 * stack, so no shadow registers are needed; but the '040 has no cross-CPU cache
 * or TLB maintenance, so those are done by IPI (call-function) here.
 */
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irq_work.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/task_stack.h>
#include <linux/percpu.h>

#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>
#include <asm/setup.h>
#include <asm/smp.h>
#include <asm/smp_e17.h>

/* ------------------------------------------------------------------------- */
/* Per-CPU pending-IPI bitmap (the software half of the doorbell).           */
/* ------------------------------------------------------------------------- */

static DEFINE_PER_CPU(unsigned long, ipi_ops);

/*
 * The hardware doorbell only signals "you have an IPI"; the message set is
 * carried in this bitmap. set_bit/xchg imply the needed barriers. On Tier-0
 * (D-cache off) this DRAM word is coherent between CPUs directly; on Tier-1 the
 * write-through + snoop keeps it coherent.
 */
void e17_ipi_set_pending(unsigned int cpu, unsigned int msg)
{
	set_bit(msg, &per_cpu(ipi_ops, cpu));
}

unsigned long e17_ipi_take_pending(unsigned int cpu)
{
	return xchg(&per_cpu(ipi_ops, cpu), 0);
}

/*
 * Non-locked peek at the pending bitmap.  The idle/tick poll uses this before
 * committing to the locked xchg above: an empty peek is a plain read (no bus
 * LOCK), so the AP's busy-poll loop does not hold the shared bus with a stream
 * of cas cycles and starve CPU0's VRAM/console writes.
 */
unsigned long e17_ipi_peek(unsigned int cpu)
{
	return READ_ONCE(per_cpu(ipi_ops, cpu));
}

/* The single cross-call hook the board backend registers. */
static void (*smp_cross_call)(const struct cpumask *, unsigned int);

void __init set_smp_cross_call(void (*fn)(const struct cpumask *, unsigned int))
{
	smp_cross_call = fn;
}

/*
 * handle_IPI() is called by the board ISR once per pending message bit after it
 * has drained e17_ipi_take_pending() and acknowledged the hardware doorbell.
 */
void handle_IPI(unsigned int ipi_msg)
{
	switch (ipi_msg) {
	case IPI_RESCHEDULE:
		scheduler_ipi();
		break;
	case IPI_CALL_FUNC:
		generic_smp_call_function_interrupt();
		break;
	case IPI_CALL_FUNC_SINGLE:
		generic_smp_call_function_single_interrupt();
		break;
	case IPI_CPU_STOP:
		local_irq_disable();
		set_cpu_online(smp_processor_id(), false);
		e17_park_secondary();		/* stop #0x2700; re-releasable */
		break;
	default:
		pr_warn("CPU%u: unknown IPI 0x%x\n", smp_processor_id(), ipi_msg);
		break;
	}
}

/* Generic senders required by the scheduler / smp_call_function core. */
void arch_smp_send_reschedule(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}

void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_CALL_FUNC_SINGLE);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_CALL_FUNC);
}

void smp_send_stop(void)
{
	struct cpumask targets;

	cpumask_copy(&targets, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &targets);
	if (!cpumask_empty(&targets))
		smp_cross_call(&targets, IPI_CPU_STOP);
}

/* ------------------------------------------------------------------------- */
/* CPU bring-up                                                              */
/* ------------------------------------------------------------------------- */

static DECLARE_COMPLETION(cpu_running);

void __init smp_init_cpus(void)
{
	/*
	 * The EUROCOM-17-5xx always has exactly two 680x0 sockets, so just mark
	 * both possible.  We deliberately do NOT walk the DT /cpus nodes here:
	 * that path (for_each_of_cpu_node -> of_get_next_cpu_node ->
	 * of_find_node_by_path, taking devtree_lock) hung on real hardware very
	 * early in setup_arch (7-seg stuck at POST 'A' == 0xfa, the checkpoint
	 * immediately before this call), while it happened to work under QEMU.
	 * It is redundant anyway: eltec/config.c:e17_smp_init_cpus() already
	 * marked both sockets possible from the board code.  Keep it simple and
	 * lock-free.
	 */
	set_cpu_possible(0, true);
	set_cpu_possible(1, true);

	/* DEBUG: POST 'b' confirms smp_init_cpus() returned.  Upper nibble must
	 * be 0 -- it is the 7-seg latch's active-low write-enable (manual). */
	if (IS_ENABLED(CONFIG_ELTEC_E17))
		*(volatile u8 *)0xfec30000 = 0x0b;
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	unsigned int cpu;

	/*
	 * Mark every socket that physically exists as *present*, independent of
	 * max_cpus.  "present" means "the CPU is populated and can be brought
	 * online"; max_cpus (the "maxcpus=" cmdline) only caps how many are
	 * onlined automatically during smp_init() at boot.  A present-but-offline
	 * CPU still gets a /sys/devices/system/cpu/cpuN device with an "online"
	 * control file, so it can be onlined later from userspace -- which is the
	 * whole point of booting "maxcpus=1" and then `echo 1 > .../cpu1/online`.
	 * (The previous "if (cpu < max_cpus)" gated presence too, so cpu1 had no
	 * device and no online node at all under maxcpus=1.)
	 */
	for_each_possible_cpu(cpu)
		set_cpu_present(cpu, true);

	/* Tier-1: turn on the boot CPU's own snoop bit early (harmless if off). */
	e17_smp_enable_snoop(0);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The generic GENERIC_CPU_DEVICES arch_register_cpu() (drivers/base/cpu.c) asks
 * this hook whether a CPU should get an "online" sysfs control file.  The weak
 * default returns false (no control file), so without this override cpu1 would
 * never expose .../cpu1/online and could not be hot-plugged.  The boot CPU
 * (logical 0, the primary socket that runs all the early single-threaded setup)
 * is deliberately kept non-removable; only the AP is hotpluggable.
 */
bool arch_cpu_is_hotpluggable(int cpu)
{
	return cpu != 0;
}
#endif

void __init smp_prepare_boot_cpu(void)
{
	/* boot CPU is logical 0; thread_info->cpu is already 0 from init_task */
}

void __init smp_cpus_done(unsigned int max_cpus)
{
}

/*
 * bringup_cpu()/cpuhp calls __cpu_up() with the freshly-created idle task.
 * We hand the AP everything it needs through the boot descriptor and release it
 * via the CPU2CON pulse (board code), then wait for it to check in.
 */
/*
 * DEBUG: primary-side progress markers.  Once the AP is active the framebuffer
 * console is unreliable (two bus masters + VRAM scanout), so the *authoritative*
 * marker is a single byte to the 7-seg (a lone I/O write that cannot bus-lock);
 * the framebuffer string is best-effort.  Only two 7-seg codes are free (0-b, e,
 * f are already used), so:
 * The AP no longer writes the 7-seg, so the boot CPU owns it here.  Final digit:
 *   5 -> wedged in the "secondary reached trampoline" print (never returned to
 *        __cpu_up); 5 is the AP trampoline's last code, left undisturbed
 *   c -> returned from e17_boot_secondary; wedged in the "waiting" printk
 *   d -> past the "waiting" printk; wedged in wait_for_completion (AP handshake
 *        / cross-CPU wakeup / timer)
 *   8 -> past wait_for_completion (AP handshake OK!); wedged in "came online"
 *        printk
 *   9 -> __cpu_up() finished -- SMP bring-up is done; wedge is later
 *   f -> the 1s completion wait TIMED OUT (AP never signalled complete())
 */
/*
 * 7-seg-only marker (no e17_early_puts: that would roll the 7-seg itself once
 * the AP is online and clobber these codes).
 */
#define P_MARK(code, s)	do { if (IS_ENABLED(CONFIG_ELTEC_E17)) \
		*(volatile u8 *)0xfec30000 = (code); } while (0)

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	int ret;

	if (!smp_cross_call) {
		pr_err("SMP: no IPI backend registered\n");
		return -EIO;
	}

	/* the AP will adopt init_mm; make sure the pgd it will load is init's */
	task_thread_info(idle)->cpu = cpu;

	pr_info("SMP: __cpu_up(%u): releasing secondary via CPU2CON...\n", cpu);
	ret = e17_boot_secondary(cpu, idle);	/* fills descriptor, pulses reset */
	if (ret) {
		pr_err("SMP: __cpu_up(%u): e17_boot_secondary failed (%d)\n",
		       cpu, ret);
		return ret;
	}

	/*
	 * printk is safe across CPUs again now that the atomic/bitops are casl-
	 * based under SMP; the P_MARK 7-seg codes stay as a belt-and-braces
	 * indicator (final digit 9 == __cpu_up done).
	 */
	/*
	 * The boot CPU writes NOTHING to the 7-seg from here on -- the AP owns it
	 * from its trampoline (POST 1..5) through secondary_start_kernel (6,7,a,e,f,
	 * then a rolling heartbeat), so its markers can't be raced/overwritten.
	 */
	pr_info("SMP: __cpu_up(%u): AP reached trampoline, waiting for online...\n",
		cpu);
	if (!wait_for_completion_timeout(&cpu_running,
					 msecs_to_jiffies(1000))) {
		pr_crit("CPU%u: failed to come online (status=%ld)\n",
			cpu, e17_ap_boot.status);
		/*
		 * The AP did not check in.  Re-assert reset (CPU2CON SRESET=0) so it is
		 * parked instead of running loose -- a secondary left executing while the
		 * boot CPU continues races the bus and can crash the boot.
		 */
		writeb(0x00, (void __iomem *)E17_CPU2CON);
		return -EIO;
	}
	/*
	 * Hand the 7-seg to the AP from here on (it owns it in secondary_start_kernel:
	 * 'd' = last boot-CPU marker before the wait, 'e' = AP past set_cpu_online,
	 * 'f' = AP enabled IRQs, rolling = AP alive in idle).  The boot CPU must NOT
	 * write the 7-seg after the handshake or it masks the AP's state.
	 */
	pr_info("SMP: CPU%u came online\n", cpu);

	/* AP is running now: safe to enable its hardware snoop bit (Tier-1). */
	e17_smp_enable_snoop(cpu);

	return 0;
}

/*
 * C entry for the secondary, reached from secondary_entry (smp_secondary.S)
 * once the MMU/caches are on and %sp/%a2 point at the AP idle task. Order
 * follows the openrisc secondary_start_kernel().
 */
/*
 * NOT __init: e17_boot_secondary() (arch/m68k/eltec/smp_e17.c) plants the
 * address of this function into the AP boot descriptor for the trampoline to
 * jump to.  Marking it __init both trips a modpost .text -> .init.text section
 * mismatch and, once CPU hotplug re-online is supported, would jump the AP into
 * freed init memory.  Keep it resident.
 */
asmlinkage void secondary_start_kernel(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int cpu;

	/*
	 * The AP owns the 7-seg from here (the boot CPU stops writing it after the
	 * handshake).  Trace: 'b' = reached C (secondary_start_kernel entry),
	 * '6' = mm joined, '7' = notify done, 'a' = complete() returned,
	 * 'e' = set_cpu_online done, 'f' = IRQs enabled, rolling = alive in idle.
	 * The AP stays printk-silent (concurrent printk wedges the console).
	 */
	*(volatile u8 *)0xfec30000 = 0x0b;	/* AP: reached C */
	e17_ap_progress = E17_AP_PROG_C;	/* boot CPU polls this */

	cpu = smp_processor_id();

	/* All kernel threads share init_mm; take a ref and join its cpumask. */
	mmgrab(mm);
	current->active_mm = mm;
	cpumask_set_cpu(cpu, mm_cpumask(mm));
	*(volatile u8 *)0xfec30000 = 0x06;	/* AP: mm joined (set_bit ok) */
	e17_ap_progress = E17_AP_PROG_MM;

	/* per-CPU IRQ / IPI doorbell + (optional) per-CPU tick come up here */
	notify_cpu_starting(cpu);
	*(volatile u8 *)0xfec30000 = 0x07;	/* AP: notify_cpu_starting done */
	e17_ap_progress = E17_AP_PROG_NOTIFY;

	/* let __cpu_up() proceed */
	complete(&cpu_running);
	*(volatile u8 *)0xfec30000 = 0x0a;	/* AP: complete() returned */
	e17_ap_progress = E17_AP_PROG_COMPLETE;

	set_cpu_online(cpu, true);
	*(volatile u8 *)0xfec30000 = 0x0e;	/* AP: set_cpu_online returned */
	e17_ap_progress = E17_AP_PROG_ONLINE;

	/*
	 * Give the secondary a real per-CPU tick: turn ON the VIC-clock (handled by
	 * e17_ap_tick on the level-4 autovector, registered by the boot CPU), and
	 * mask the unused LEB source so it can't storm.  The tick lets the AP report
	 * RCU quiescent states / run the scheduler tick, which it needs to finish
	 * the cpuhp online states.
	 */
	writeb(E17_SICF_LEB_DIS, (void __iomem *)E17_CPU2CON);	/* mask unused LEB */
	e17_ap_tick_enable();					/* enable VIC-clock tick */
	local_irq_enable();
	*(volatile u8 *)0xfec30000 = 0x0f;
	e17_ap_progress = E17_AP_PROG_IDLE;

	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);	/* never returns */
}

#ifdef CONFIG_HOTPLUG_CPU
/* ------------------------------------------------------------------------- */
/* CPU hot(un)plug.  Lets CPU1 be brought online at runtime -- boot with     */
/* "maxcpus=1" to a shell/telnet, then: echo 1 > .../cpu1/online, with the    */
/* console + dmesg available to watch the AP bring-up.                        */
/* ------------------------------------------------------------------------- */

/* Called ON the CPU being taken down. */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();

	set_cpu_online(cpu, false);
	irq_migrate_all_off_this_cpu();
	clear_tasks_mm_cpumask(cpu);
	return 0;
}

/* Called on the boot CPU after the dying CPU parked; hold it in reset so a  */
/* later online cleanly re-releases it (CPU2CON edge 0->1 resets to the      */
/* reset vector).  HOTPLUG_CORE_SYNC_DEAD does the actual wait for DEAD.     */
void __cpu_die(unsigned int cpu)
{
	/*
	 * Turn the secondary's hardware snoop OFF before it stops running: a CPU
	 * held in reset has an invalid /MI output, so leaving its snoop enabled
	 * corrupts snoop cycles on the shared bus (HW manual).  Only then reset it.
	 */
	e17_smp_disable_snoop(cpu);
	writeb(0x00, (void __iomem *)E17_CPU2CON);	/* re-assert secondary reset */
}

/* The dying (secondary) CPU's last act: report dead, then park with all     */
/* interrupts masked.  A future online re-releases it via CPU2CON, which     */
/* resets it back to secondary_entry (the trampoline), so this never returns.*/
void __noreturn arch_cpu_idle_dead(void)
{
	idle_task_exit();
	cpuhp_ap_report_dead();
	e17_park_secondary();
	BUG();
}
#endif /* CONFIG_HOTPLUG_CPU */

/* ------------------------------------------------------------------------- */
/* Cross-CPU TLB shootdown (the '040 has no broadcast pflush).               */
/* ------------------------------------------------------------------------- */

/*
 * The 68040/060 has no cross-CPU pflush, and its per-address 'pflush' only
 * targets the *current* CPU's ATC/context. A remote CPU asked to flush a mapping
 * for some other mm cannot do a precise pflush (its context register differs),
 * and the existing local_flush_tlb_mm/range are guarded by "== active_mm" so
 * they would silently skip on the remote CPU. The correct, simple primitive is
 * therefore: every CPU that could hold the entry does a full ATC flush
 * (local_flush_tlb_all == pflusha). Over-broad but always correct; per-address
 * remote flushing is a later optimization.
 */
static void ipi_flush_tlb_all(void *ignored)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	on_each_cpu(ipi_flush_tlb_all, NULL, 1);
}

/*
 * User-mm shootdowns.  These MUST reach every CPU that could hold a stale ATC
 * entry for the mm.  The obvious optimisation -- scope to mm_cpumask(mm) -- is a
 * trap on this port: the classic-m68k switch_mm()/activate_mm() never populate
 * mm_cpumask for user mms (only init_mm gets a bit, in secondary_start_kernel),
 * so mm_cpumask is empty and on_each_cpu_mask() would flush NOBODY -- not even
 * the local CPU.  That left stale 68040 ATC entries alive across fork/munmap/
 * mprotect/reclaim and corrupted unrelated processes (a freed page still mapped
 * writable on the other CPU gets handed out as someone's stack).  Broadcast to
 * all online CPUs unconditionally, exactly like flush_tlb_all(); it is trivially
 * correct and cheap for a 2-CPU board.  (Re-introduce mm_cpumask scoping only
 * once switch_mm() actually maintains the mask.)
 */
void flush_tlb_mm(struct mm_struct *mm)
{
	on_each_cpu(ipi_flush_tlb_all, NULL, 1);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	on_each_cpu(ipi_flush_tlb_all, NULL, 1);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	on_each_cpu(ipi_flush_tlb_all, NULL, 1);
}

/* Kernel mappings are global: flush every online CPU's ATC. */
void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	on_each_cpu(ipi_flush_tlb_all, NULL, 1);
}

/* ------------------------------------------------------------------------- */
/* Cross-CPU I-cache invalidation (cpush/cinv are local on the '040).        */
/* ------------------------------------------------------------------------- */

static void ipi_icache_all(void *ignored)
{
	/* remote CPUs only need to drop stale I-lines; data already in DRAM */
	__asm__ __volatile__("nop\n\t"
			     ".chip 68040\n\t"
			     "cinva %ic\n\t"
			     ".chip 68k");
}

/*
 * Called (in addition to the local flush already done by mm/cache.c) whenever
 * code visible to other CPUs is modified: module load, kprobes, ptrace poke.
 * Hook this from flush_icache_range()/flush_icache_user_range() under SMP.
 */
void smp_flush_icache_all(void)
{
	on_each_cpu(ipi_icache_all, NULL, 1);
}
