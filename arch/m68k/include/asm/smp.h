/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/m68k/include/asm/smp.h
 *
 * SMP support for Linux/m68k. Presently implemented only for the ELTEC
 * EUROCOM-17-5xx dual-68040/68060 board (CONFIG_SMP depends on ELTEC_E17).
 */
#ifndef _ASM_M68K_SMP_H
#define _ASM_M68K_SMP_H

#ifdef CONFIG_SMP
#ifndef __ASSEMBLY__

#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/thread_info.h>

/*
 * On MMU m68k 'current' is register %a2 and current_thread_info() is derived
 * from the stack pointer (asm/current.h, asm/thread_info.h). Both are naturally
 * per-CPU: each CPU has its own %a2 and runs on its own kernel stack. So the
 * logical cpu id is simply the idle/task thread_info->cpu - no dedicated CPU-id
 * register and no per-CPU scratch/shadow registers are needed (unlike openrisc
 * or hexagon).
 */
#define raw_smp_processor_id()	(current_thread_info()->cpu)

/* the boot CPU is always logical cpu 0 (the primary socket) */
#define hard_smp_processor_id()	raw_smp_processor_id()

/* IPI message set, mapped to the board doorbells in eltec/smp_e17.c */
enum ipi_msg_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
	NR_IPI
};

struct task_struct;

/* arch/m68k/kernel/smp.c */
asmlinkage void secondary_start_kernel(void);
void handle_IPI(unsigned int ipi_msg);
void smp_init_cpus(void);		/* called from setup_arch() */
/* Broadcast I-cache invalidation to all CPUs (the '040 cinva/cpush is local).
 * Hooked from flush_icache_range()/flush_icache_user_page() in mm/cache.c. */
void smp_flush_icache_all(void);

/* set by the board IPI backend at init (openrisc-style single hook) */
void set_smp_cross_call(void (*fn)(const struct cpumask *, unsigned int));

/*
 * Cross-CPU senders required by the generic smp_call_function core
 * (kernel/smp.c). Like every other SMP arch, m68k must declare these in its
 * asm/smp.h; they are defined in arch/m68k/kernel/smp.c.
 * (arch_smp_send_reschedule() is already declared by <linux/smp.h>.)
 */
void arch_send_call_function_single_ipi(int cpu);
void arch_send_call_function_ipi_mask(const struct cpumask *mask);

#ifdef CONFIG_HOTPLUG_CPU
int __cpu_disable(void);
void __cpu_die(unsigned int cpu);
#endif

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_SMP */
#endif /* _ASM_M68K_SMP_H */
