/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/m68k/include/asm/smp_e17.h
 *
 * ELTEC EUROCOM-17-5xx dual-68040/68060 SMP hardware definitions.
 *
 * Register addresses and bit meanings are from the EUROCOM-17-5xx hardware
 * manual (Tables 27, 50, 51, 52, 53 and section 3.19) cross-checked against the
 * QEMU e17 model (hw/m68k/e17.c, hw/misc/e17_sysc.c) and the RMON ROM.
 */
#ifndef _ASM_M68K_SMP_E17_H
#define _ASM_M68K_SMP_E17_H

/* ---- Local-I/O window control registers (manual Table 27) ---------------- */

/*
 * CPU2CON - Secondary CPU Control Register, byte, read/write ($FEC5.8000).
 * Access view differs by CPU (manual Tables 50 [primary] and 51 [secondary]).
 */
#define E17_CPU2CON		0xfec58000

/* --- CPU2CON as seen by the PRIMARY (Table 50) --- */
#define E17_CPU2CON_MIPEND	0x80	/* r: mailbox IRQ to secondary pending  */
#define E17_CPU2CON_CPUID	0x40	/* r: reads 1 on primary, 0 on secondary*/
#define E17_CPU2CON_SRESET	0x20	/* w: 0=hold secondary in reset, 1=run  */
#define E17_CPU2CON_SIPL_MASK	0x07	/* w: IPL driven into secondary (IPI)   */
/* Writing SIPL[2:0] = level (1..7) raises an autovector IPI on the secondary. */

/* --- CPU2CON as seen by the SECONDARY (Table 51) --- */
#define E17_CPU2CON_VICIRQ	0x20	/* r: VIC clock IRQ pending             */
#define E17_CPU2CON_SICF_MASK	0x07	/* w: secondary IRQ control function    */
#define E17_SICF_VICCLK_DIS	0x00	/* disable VIC-clock IRQ                 */
#define E17_SICF_VICCLK_EN	0x01	/* enable  VIC-clock IRQ (per-CPU tick) */
#define E17_SICF_LEB_DIS	0x02
#define E17_SICF_LEB_EN		0x03
#define E17_SICF_IACK_VIC	0x04	/* IACK the VIC(-clock) IRQ             */
#define E17_SICF_IACK_MBOX	0x05	/* IACK the mailbox IRQ (clears SIPL)   */
#define E17_SICF_VME_DIS	0x06
#define E17_SICF_VME_EN		0x07

/*
 * SNCR - Snoop Control Register, byte, WRITE-ONLY ($FEC5.E000), Tables 52/53.
 * Per-CPU SC1/SC0 pair. SC=01 => "supply dirty & sink" (the coherent mode for
 * write-through shared data). Must keep a software shadow (write-only!).
 */
#define E17_SNCR		0xfec5e000
#define E17_SNCR_PRI_SC0	0x01
#define E17_SNCR_PRI_SC1	0x02
#define E17_SNCR_SEC_SC0	0x04
#define E17_SNCR_SEC_SC1	0x08
#define E17_SNCR_PRI_SNOOP	(E17_SNCR_PRI_SC0)		/* SC=01 */
#define E17_SNCR_SEC_SNOOP	(E17_SNCR_SEC_SC0)		/* SC=01 */

/* ESR - enable slave select (VME decoders), not used by SMP ($FEC5.C000). */
#define E17_ESR			0xfec5c000

/*
 * VIC068A interprocessor-communication (manual section 3.19.2):
 *  ICGS switch register  $FEC0.105F : secondary writes a bit 0->1 to IRQ primary
 *  ICMS interrupt-ctrl   $FEC0.1047 : mask; the ICGS bit fires when its ICMS
 *                                     control bit is 0. ICGS group IRQ = level 6.
 */
#define E17_VIC_BASE		0xfec00000
#define E17_VIC_ICGS		0xfec0105f
#define E17_VIC_ICMS_ICR	0xfec01047

/* One dedicated ICGS bit used as the secondary->primary doorbell. */
#define E17_ICGS_IPI_BIT	0x01

/* IPL/level choices (manual Table 48). */
#define E17_IPI_SEC_IPL		6	/* primary->secondary: SIPL level        */
#define E17_IPI_PRI_LEVEL	6	/* secondary->primary: ICGS group, lvl 6 */
#define E17_SEC_TICK_LEVEL	4	/* secondary VIC-clock tick, autovector  */

/* ---- Memory: cache-inhibited RAM mirror (manual Table 26 / section 3.2.4) - */
/*
 * $1000.0000-$1FFF.FFFF is a non-cacheable alias of DRAM/VRAM. OR this bit into
 * a DRAM physical address to reach it uncached from either CPU regardless of
 * cache state - used for the AP boot descriptor and (Tier-0-safe) IPI bitmaps.
 */
#define E17_RAM_MIRROR		0x10000000UL
#define e17_uncached_phys(pa)	(((unsigned long)(pa)) | E17_RAM_MIRROR)

/* Secondary reset vector lives at the very bottom of DRAM (680x0 reset). */
#define E17_AP_RESET_SSP	0x0	/* DRAM[0] <- initial SSP                */
#define E17_AP_RESET_PC		0x4	/* DRAM[4] <- initial PC (secondary_entry)*/
#define E17_AP_DESC_PTR		0x8	/* DRAM[8] <- &e17_ap_boot (uncached ptr) */

/*
 * Byte offsets of struct e17_ap_boot fields, consumed by smp_secondary.S.
 * MUST match the struct below; smp_e17.c has a BUILD_BUG_ON() guard.
 */
#define E17_APB_ENTRY		0x00
#define E17_APB_STACK		0x04
#define E17_APB_CURRENT		0x08
#define E17_APB_VBR		0x0c
#define E17_APB_SRP		0x10
#define E17_APB_URP		0x14
#define E17_APB_TC		0x18
#define E17_APB_ITT0		0x1c
#define E17_APB_ITT1		0x20
#define E17_APB_DTT0		0x24
#define E17_APB_DTT1		0x28
#define E17_APB_CACR		0x2c
#define E17_APB_STATUS		0x30
#define E17_APB_CPU		0x34
#define E17_APB_EARLY_VBR	0x38
#define E17_APB_FAULT_PC	0x3c
#define E17_APB_FAULT_VEC	0x40

/*
 * Number of longwords the boot CPU pre-pushes onto the AP's page stack for the
 * AP entry code (smp_secondary.S: ap_page_code) to pop.  Shared by the asm and
 * the C that builds the stack (smp_e17.c).  Kept outside __ASSEMBLY__ guards.
 */
#define E17_AP_NPOP		12

/*
 * Offset within the AP's page0 of the kernel-entry descriptor the boot CPU fills
 * (12 longwords: cacr, tc, srp, urp, itt0, itt1, dtt0, dtt1, vbr, stack, current,
 * entry).  The AP reads it (VBR points at page0) when it enables the MMU and
 * jumps into secondary_start_kernel.  Clear of the blob (~0x600) and the stack.
 */
#define E17_AP_DESC_OFF		0xc00

/*
 * After the kernel-entry values, the boot CPU pushes ONE more longword: the
 * physical address of a shared scratch long, used for the cross-CPU coherency
 * ping-pong.  Each round the boot CPU writes a fresh value there and IPIs the
 * AP; the AP reads it, writes back its bitwise inverse, and acks; the boot CPU
 * checks the inverse.  (Both CPUs run D-cache off, so writes land in DRAM.)
 */

/*
 * Magic values the AP's interrupt handlers publish to the shared scratch long so
 * the boot CPU can confirm each real interrupt was actually TAKEN (not polled).
 */
#define E17_AP_IPI_IRQ_MAGIC	0x1a1b1c1d	/* level-6 mailbox IPI handler ran */
#define E17_AP_VIC_IRQ_MAGIC	0x2a2b2c2d	/* level-4 VIC-clock handler ran   */

/*
 * MMU-validation magic: after enabling its MMU the AP writes this to a kernel
 * VIRTUAL address the boot CPU provides (desc[12]); the boot CPU reads it back to
 * confirm the secondary's MMU translates correctly before we commit to entering
 * secondary_start_kernel.
 */
#define E17_AP_MMU_MAGIC	0x3c3d3e3f

/*
 * FPU presence probe: very early in the trampoline (while the AP still uses its
 * own private vector table) the AP hooks the line-F vector, executes an fnop,
 * and publishes one of these into the shared scratch slot E17_SCR_FPU.  The boot
 * CPU reads it back and reports whether the secondary actually has an FPU -- the
 * secondary's first context switch faults in resume()'s fsave/frestore if not.
 */
#define E17_AP_FPU_PRESENT	0x4f4b4f4b	/* "OKOK": fnop executed -> FPU present */
#define E17_AP_FPU_ABSENT	0x4e4f4e4f	/* "NONO": fnop line-F trapped -> no FPU */

/*
 * e17_ap_boot.status handshake values.  RESERVED: the current trampoline does
 * NOT write this field -- AP liveness is proven by the mailbox-ack handshake and
 * the e17_ap_progress milestones instead -- so smp.c's failure message prints the
 * initial value.  Kept for a future trampoline-side status store.
 */
#define E17_AP_HELD	0
#define E17_AP_BOOTING	1
#define E17_AP_ALIVE	2

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * AP boot descriptor. The 680x0 reset only conveys SSP (DRAM[0]) and PC
 * (DRAM[4]); everything else the secondary needs to reach virtual space is
 * passed here. The boot CPU fills it; the AP reads it THROUGH THE UNCACHED
 * MIRROR (e17_uncached_phys) so no cache flush is needed between the two CPUs.
 *
 * Field offsets are consumed by smp_secondary.S - keep in sync with the
 * E17_APB_* asm-offsets below if you change the layout.
 */
struct e17_ap_boot {
	unsigned long	entry_virt;	/* 0x00 virtual secondary_start_kernel  */
	unsigned long	stack_virt;	/* 0x04 virtual top of the AP idle stack*/
	unsigned long	current_task;	/* 0x08 AP idle task_struct (-> %a2)    */
	unsigned long	vbr;		/* 0x0c vector base register            */
	unsigned long	srp;		/* 0x10 supervisor root pointer         */
	unsigned long	urp;		/* 0x14 user root pointer               */
	unsigned long	tc;		/* 0x18 translation control (TC.E set)  */
	unsigned long	itt0;		/* 0x1c instruction transparent transl. */
	unsigned long	itt1;		/* 0x20                                 */
	unsigned long	dtt0;		/* 0x24 data transparent translation    */
	unsigned long	dtt1;		/* 0x28                                 */
	unsigned long	cacr;		/* 0x2c cache control to load           */
	volatile long	status;		/* 0x30 AP_* handshake (via mirror)     */
	unsigned long	cpu;		/* 0x34 logical cpu id for this AP      */
	unsigned long	early_vbr;	/* 0x38 phys addr of the AP fault-catch VBR table */
	volatile long	fault_pc;	/* 0x3c AP fault: PC captured by ap_capture */
	volatile long	fault_vec;	/* 0x40 AP fault: vector offset captured    */
};

extern struct e17_ap_boot e17_ap_boot;

/*
 * secondary_start_kernel() progress, written by the AP and polled by the boot
 * CPU (tick-/console-independent) so we can see exactly how far C entry gets.
 */
extern volatile u32 e17_ap_progress;
#define E17_AP_PROG_C		1	/* reached secondary_start_kernel (C)   */
#define E17_AP_PROG_MM		2	/* joined init_mm cpumask               */
#define E17_AP_PROG_NOTIFY	3	/* notify_cpu_starting() returned       */
#define E17_AP_PROG_COMPLETE	4	/* complete(&cpu_running) done          */
#define E17_AP_PROG_ONLINE	5	/* set_cpu_online() done                */
#define E17_AP_PROG_IDLE	6	/* tick+IRQs on, entering cpu idle loop */

/* board glue (arch/m68k/eltec/smp_e17.c) */
void e17_smp_init_cpus(void);
int  e17_boot_secondary(unsigned int cpu, struct task_struct *idle);
void e17_send_ipi(const struct cpumask *mask, unsigned int ipi_msg);
void e17_ipi_ack_self(void);		/* ack the local doorbell after draining */
void e17_smp_enable_snoop(unsigned int cpu);
void e17_smp_disable_snoop(unsigned int cpu);
void e17_park_secondary(void);		/* stop #0x2700 park (offline/stop)      */
void e17_ap_tick_enable(void);		/* secondary: turn on its VIC-clock tick */

/*
 * Drain and dispatch any locally-pending IPI bits.  With CONFIG_E17_SMP_HW_IPI
 * the primary->secondary mailbox doorbell is a real level-6 IRQ (e17_ipi_isr),
 * so on the secondary this is only a backstop; the secondary->primary direction
 * has no modelled doorbell yet, so the primary always polls it from its idle
 * loop and timer tick.  Poll-only builds use this for both directions.
 */
void e17_ipi_poll(void);

/* implemented in arch/m68k/kernel/smp.c, called by the board ISRs */
void handle_IPI(unsigned int ipi_msg);
unsigned long e17_ipi_take_pending(unsigned int cpu);	/* xchg(ops,0)          */
unsigned long e17_ipi_peek(unsigned int cpu);		/* plain read, no LOCK  */
void e17_ipi_set_pending(unsigned int cpu, unsigned int msg);

/* asm blob copied into the AP's page0 (smp_secondary.S): a 1KiB vector table,
 * the reset entry point, and the shared exception handler. */
extern char ap_blob_start[], ap_blob_end[], ap_entry[], ap_exc_handler[];

#endif /* __ASSEMBLY__ */
#endif /* _ASM_M68K_SMP_E17_H */
