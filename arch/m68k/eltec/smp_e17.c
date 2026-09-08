// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/m68k/eltec/smp_e17.c
 *
 * ELTEC EUROCOM-17-5xx board glue for SMP: secondary bring-up over CPU2CON
 * ($FEC58000), the bidirectional hardware IPI doorbells (CPU2CON SIPL and the
 * VIC ICMS switch $FEC0105F), and the SNCR ($FEC5E000) snoop-mode management.
 *
 * See DESIGN.md for the full rationale and the manual citations.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/kernel_stat.h>

#include <asm/cacheflush.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/kmap.h>
#include <asm/smp.h>
#include <asm/smp_e17.h>
#include <asm/irq.h>

/*
 * The shared AP boot descriptor. Ordinary kernel data; the AP reads it through
 * the non-cacheable RAM mirror (e17_uncached_phys) so no cache flush is needed.
 * Cache-line aligned so a Tier-1 write-through push/snoop stays self-contained.
 */
struct e17_ap_boot e17_ap_boot __cacheline_aligned;

/* Written by the AP in secondary_start_kernel(), polled by the boot CPU. */
volatile u32 e17_ap_progress;

/*
 * The 680x0 reset protocol makes the secondary fetch its SSP and PC from
 * physical DRAM 0 and 4 (and we stash the descriptor pointer at DRAM 8). The
 * boot CPU must write those words, but it CANNOT use __va(0): the kernel leaves
 * virtual page 0 unmapped to trap NULL dereferences (see head.S), so *(u32*)0
 * faults. Map the low DRAM words with an explicit uncached ioremap for the
 * brief window we plant the reset vector; uncached also means the write lands
 * in DRAM immediately, which is what the AP (MMU/caches off) reads.
 */
#define DRAM_LOW_SIZE	16
#define DRAM_LOW(dl, off)	((dl)[(off) / 4])

/* ------------------------------------------------------------------------- */
/* Read the boot CPU's MMU/cache control regs so the AP can match exactly.    */
/* ------------------------------------------------------------------------- */

#define MOVEC_FROM(reg)						\
({								\
	unsigned long __v;					\
	__asm__ __volatile__(".chip 68040\n\t"			\
			     "movec %%" #reg ",%0\n\t"		\
			     ".chip 68k" : "=d" (__v));		\
	__v;							\
})

/* The AP's private bring-up area: two contiguous pages.  page0 = exception
 * vector table + entry code + handler; page1 = the pre-built pop-stack.
 * Allocated once and reused across (re)online attempts. */
static void *e17_ap_pages;

/*
 * Shared scratch longs (the boot CPU pushes the base address to the AP):
 *   [0] cross-CPU coherency ping-pong (boot writes V, AP writes ~V, boot checks)
 *   [1] IPI-interrupt test result   (AP's level-6 handler publishes its magic)
 *   [2] VIC-clock interrupt result  (AP's level-4 handler publishes its magic)
 * Separate slots so the two interrupt handlers don't overwrite each other's
 * result before the boot CPU has read it.
 */
static u32 e17_ap_scratch[4];
#define E17_SCR_COH	0
#define E17_SCR_IPI	1
#define E17_SCR_VIC	2
#define E17_SCR_FPU	3	/* AP publishes E17_AP_FPU_PRESENT / _ABSENT here */
#define E17_COH_ROUNDS	256

/*
 * A vmalloc'd page whose kernel VIRTUAL address is NOT identity-mapped (it lives
 * in the vmalloc region, reachable only via a page-table walk).  The boot CPU
 * hands the AP this vaddr; after the AP enables its MMU it writes the MMU magic
 * there, proving its translation matches the primary's before we let it into C.
 */
static u32 *e17_ap_mmu_test;

/*
 * Compute the values the AP needs to enter the kernel and stash them in the
 * shared struct.  The new page-based bring-up copies these onto the AP's stack
 * (below); the AP pops them.  (Kept as a struct for the eventual kernel-entry
 * step; today the AP only pops-and-discards them to prove the stack.)
 */
static void e17_fill_descriptor(unsigned int cpu, struct task_struct *idle)
{
	struct e17_ap_boot *apb = &e17_ap_boot;

	/* Compile-time guard: asm offsets in smp_e17.h must match the struct. */
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, entry_virt) != E17_APB_ENTRY);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, stack_virt) != E17_APB_STACK);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, current_task) != E17_APB_CURRENT);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, vbr) != E17_APB_VBR);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, srp) != E17_APB_SRP);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, tc) != E17_APB_TC);
	BUILD_BUG_ON(offsetof(struct e17_ap_boot, cacr) != E17_APB_CACR);

	apb->entry_virt   = (unsigned long)secondary_start_kernel;
	apb->stack_virt   = (unsigned long)task_stack_page(idle) + THREAD_SIZE;
	apb->current_task = (unsigned long)idle;
	apb->vbr          = MOVEC_FROM(vbr);
	apb->srp          = MOVEC_FROM(srp);
	apb->urp          = MOVEC_FROM(urp);
	apb->tc           = MOVEC_FROM(tc);	/* E-bit already set on boot CPU */
	apb->itt0         = MOVEC_FROM(itt0);
	apb->itt1         = MOVEC_FROM(itt1);
	apb->dtt0         = MOVEC_FROM(dtt0);
	apb->dtt1         = MOVEC_FROM(dtt1);
	apb->cacr         = MOVEC_FROM(cacr);
	apb->cpu          = cpu;
}

/*
 * Ring the CPU2CON mailbox doorbell (write SIPL, keep SRESET set) and wait,
 * bounded, for the secondary to ack it -- the AP acks by writing SICF=IACK_MBOX,
 * which clears MIPEND.  Returns true if acked within @ms.
 */
static bool e17_ping_secondary(void __iomem *cpu2con, unsigned int ms)
{
	unsigned long tmo = jiffies + msecs_to_jiffies(ms);

	writeb(E17_CPU2CON_SRESET | E17_IPI_SEC_IPL, cpu2con);
	while (time_before(jiffies, tmo)) {
		if (!(readb(cpu2con) & E17_CPU2CON_MIPEND))
			return true;
		cpu_relax();
	}
	return false;
}

/* Wait (bounded) for the AP to publish @want into scratch slot @idx. */
static bool e17_wait_scratch(int idx, u32 want, unsigned int ms)
{
	unsigned long tmo = jiffies + msecs_to_jiffies(ms);

	do {
		rmb();
		if (READ_ONCE(e17_ap_scratch[idx]) == want)
			return true;
		cpu_relax();
	} while (time_before(jiffies, tmo));
	return false;
}

/* ------------------------------------------------------------------------- */
/* Secondary release: page-based bring-up (CPU2CON $FEC58000 pulse).          */
/* ------------------------------------------------------------------------- */

int e17_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	void __iomem *cpu2con = (void __iomem *)E17_CPU2CON;
	extern char ap_blob_start[], ap_blob_end[], ap_entry[], ap_exc_handler[];
	extern char ap_ipi_handler[], ap_vic_handler[];
	volatile u32 *dl;
	u32 *base, *sp, *vt;
	unsigned long base_phys, sp_phys, entry_phys, handler_phys, bloblen;
	bool acked;
	u32 saved0, saved4;
	int i;

	if (cpu != 1)			/* only one secondary socket on this board */
		return -EINVAL;

	/*
	 * Allocate TWO contiguous pages once; reuse on re-online.
	 *   page0: exception vector table (1 KiB) + ap_entry + ap_exc_handler
	 *   page1: the pre-built pop-stack (top)
	 */
	if (!e17_ap_pages) {
		e17_ap_pages = (void *)__get_free_pages(GFP_KERNEL, 1);	/* 2 pages */
		if (!e17_ap_pages) {
			pr_err("SMP: cannot allocate AP bring-up pages\n");
			return -ENOMEM;
		}
	}
	base = e17_ap_pages;
	memset(base, 0, 2 * PAGE_SIZE);
	base_phys = __pa(base);

	/* Non-identity kernel vaddr for the AP MMU-validation write (once). */
	if (!e17_ap_mmu_test) {
		e17_ap_mmu_test = vmalloc(PAGE_SIZE);
		if (!e17_ap_mmu_test) {
			pr_err("SMP: cannot vmalloc AP MMU-test page\n");
			return -ENOMEM;
		}
	}

	e17_fill_descriptor(cpu, idle);		/* compute the kernel-entry values */

	/* Copy the blob (vector-table space + ap_entry + ap_exc_handler) to page0. */
	bloblen = ap_blob_end - ap_blob_start;
	if (bloblen > E17_AP_DESC_OFF) {
		/* the kernel-entry descriptor lives at page0 + E17_AP_DESC_OFF;
		 * a blob overrunning it would be silently corrupted. */
		pr_err("SMP: AP blob too large (%lu bytes)\n", bloblen);
		return -EINVAL;
	}
	memcpy(base, ap_blob_start, bloblen);
	entry_phys   = base_phys + (ap_entry - ap_blob_start);
	handler_phys = base_phys + (ap_exc_handler - ap_blob_start);

	/*
	 * Fill the 256-entry exception vector table at the start of page0: every
	 * vector points at ap_exc_handler (physical), so ANY exception the AP takes
	 * once it loads VBR lands in our own handler, which shows it on the 7-seg.
	 */
	vt = base;
	for (i = 0; i < 256; i++)
		vt[i] = handler_phys;
	/*
	 * Install the two real IRQ-test handlers at their autovectors (24+level):
	 * the mailbox IPI is level 6 (vector 30), the VIC clock is level 4 (vector
	 * 28).  Everything else still lands in ap_exc_handler.
	 */
	vt[24 + E17_IPI_SEC_IPL]    = base_phys + (ap_ipi_handler - ap_blob_start);
	vt[24 + E17_SEC_TICK_LEVEL] = base_phys + (ap_vic_handler - ap_blob_start);

	/*
	 * Kernel-entry descriptor: the values the AP loads to enable the MMU and jump
	 * into secondary_start_kernel (see ap_entry .Lentry).  Order matches the asm
	 * offsets.
	 */
	{
		u32 *desc = (u32 *)((char *)base + E17_AP_DESC_OFF);

		desc[0]  = e17_ap_boot.cacr;
		desc[1]  = e17_ap_boot.tc;
		desc[2]  = e17_ap_boot.srp;
		desc[3]  = e17_ap_boot.urp;
		desc[4]  = e17_ap_boot.itt0;
		desc[5]  = e17_ap_boot.itt1;
		desc[6]  = e17_ap_boot.dtt0;
		desc[7]  = e17_ap_boot.dtt1;
		desc[8]  = e17_ap_boot.vbr;
		desc[9]  = e17_ap_boot.stack_virt;
		desc[10] = e17_ap_boot.current_task;
		desc[11] = e17_ap_boot.entry_virt;
		desc[12] = (u32)(unsigned long)e17_ap_mmu_test;	/* kernel vaddr to test */
	}

	/*
	 * Build the pop-stack at the TOP of page1: E17_AP_NPOP longwords the AP pops
	 * (order = the eventual kernel-entry order; discarded for now to prove the
	 * stack).  SSP points at the first (lowest address).
	 */
	sp = (u32 *)((char *)base + 2 * PAGE_SIZE) - (E17_AP_NPOP + 1);
	sp[0]  = e17_ap_boot.cacr;
	sp[1]  = e17_ap_boot.tc;
	sp[2]  = e17_ap_boot.srp;
	sp[3]  = e17_ap_boot.urp;
	sp[4]  = e17_ap_boot.itt0;
	sp[5]  = e17_ap_boot.itt1;
	sp[6]  = e17_ap_boot.dtt0;
	sp[7]  = e17_ap_boot.dtt1;
	sp[8]  = e17_ap_boot.vbr;
	sp[9]  = e17_ap_boot.current_task;
	sp[10] = e17_ap_boot.stack_virt;
	sp[11] = e17_ap_boot.entry_virt;
	sp[E17_AP_NPOP] = __pa(&e17_ap_scratch[0]);	/* shared scratch base */
	sp_phys = base_phys + 2 * PAGE_SIZE - (E17_AP_NPOP + 1) * 4;

	memset(e17_ap_scratch, 0, sizeof(e17_ap_scratch));	/* AP fills the slots */
	wmb();

	/* Plant the two mandatory 680x0 reset words: SSP (DRAM[0]) and PC (DRAM[4]);
	 * the boot CPU runs from the kernel's own VBR table so these physical-0 words
	 * are not its live vectors -- save/restore only to be tidy. */
	dl = (volatile u32 __force *)ioremap(0, DRAM_LOW_SIZE);
	if (!dl) {
		pr_err("SMP: cannot map low DRAM for the reset vector\n");
		return -ENOMEM;
	}

	writeb(0x00, cpu2con);			/* hold the secondary in reset */
	saved0 = DRAM_LOW(dl, E17_AP_RESET_SSP);
	saved4 = DRAM_LOW(dl, E17_AP_RESET_PC);
	DRAM_LOW(dl, E17_AP_RESET_SSP) = sp_phys;	/* initial SSP -> pop-stack top */
	DRAM_LOW(dl, E17_AP_RESET_PC)  = entry_phys;	/* initial PC  -> ap_entry     */
	wmb();

	pr_info("SMP: AP pages @phys 0x%08lx  vbr=0x%08lx  entry=0x%08lx  handler=0x%08lx  SSP=0x%08lx\n",
		base_phys, base_phys, entry_phys, handler_phys, sp_phys);
	pr_info("SMP: releasing secondary -- it shows '0' then polls for the mailbox IPI ('E'+digit on fault)\n");

	*(volatile u8 *)0xfec30000 = 0x00;	/* clear; the AP will animate it */
	writeb(E17_CPU2CON_SRESET, cpu2con);	/* release: SRESET 0 -> 1 */

	udelay(200);				/* let the AP consume the reset vectors */
	DRAM_LOW(dl, E17_AP_RESET_SSP) = saved0;
	DRAM_LOW(dl, E17_AP_RESET_PC)  = saved4;
	iounmap((void __iomem *)dl);

	/*
	 * Bring-up handshake test (boot CPU side).  Give the AP a few ms to reset and
	 * reach its poll loop (showing '0'), then:
	 *   1. Ring the mailbox (IPI #1) and wait for the ack -> proves the boot->AP
	 *      doorbell path.  The AP then shows '1'.
	 *   2. Ring it again (IPI #2) and wait for the ack.  The AP only reaches its
	 *      second mailbox poll AFTER its own VIC-clock has ticked (it shows '2'
	 *      in between), so ack #2 proves the secondary can see its own clock
	 *      source.  The AP then shows '3'.
	 * These are pre-flight checks: if any fails we jump to 'stop', park the AP and
	 * return an error.  If they all pass we fall through to the kernel-entry stages
	 * below (come-in, MMU validation, secondary_start_kernel) and bring cpu%u up.
	 */
	mdelay(5);				/* AP: reset + reach the poll loop */

	acked = e17_ping_secondary(cpu2con, 3000);	/* IPI #1 */
	if (!acked) {
		pr_err("SMP: IPI #1 not acked in 3s -- boot->AP doorbell FAILED\n");
		goto stop;
	}
	pr_info("SMP: IPI #1 acked -- boot->AP mailbox doorbell works\n");

	/*
	 * FPU presence probe result.  The AP ran this before its handshake, so the
	 * slot is populated by now.  This tells us whether the secondary has an FPU
	 * at all -- if not, resume()'s fsave/frestore faults on the first context
	 * switch (which is what wedged us).
	 */
	{
		u32 fpu = READ_ONCE(e17_ap_scratch[E17_SCR_FPU]);

		if (fpu == E17_AP_FPU_PRESENT)
			pr_info("SMP: AP FPU probe: PRESENT (fnop executed cleanly)\n");
		else if (fpu == E17_AP_FPU_ABSENT)
			pr_info("SMP: AP FPU probe: ABSENT (fnop took a line-F trap -- no FPU on the secondary!)\n");
		else
			pr_info("SMP: AP FPU probe: inconclusive (scratch=0x%08x)\n", fpu);
	}

	acked = e17_ping_secondary(cpu2con, 3000);	/* IPI #2 (after the VIC check) */
	if (!acked) {
		pr_err("SMP: IPI #2 not acked in 3s -- secondary's VIC clock NOT ticking (stuck at '1')\n");
		goto stop;
	}
	pr_info("SMP: IPI #2 acked -- secondary sees its own VIC clock (tick works)\n");

	/*
	 * Interrupt test 1: does the AP take the mailbox as a REAL interrupt?  The AP
	 * has installed our handler at the level-6 autovector and unmasked level 6;
	 * fire the doorbell and wait for the handler to publish its magic (it also
	 * acks).  Unlike the polled checks above, this only passes if the interrupt
	 * was actually delivered and taken.
	 */
	writeb(E17_CPU2CON_SRESET | E17_IPI_SEC_IPL, cpu2con);	/* trigger the IPI */
	if (!e17_wait_scratch(E17_SCR_IPI, E17_AP_IPI_IRQ_MAGIC, 3000)) {
		pr_err("SMP: IPI INTERRUPT FAILED -- AP never took the level-6 mailbox IRQ\n");
		goto stop;
	}
	pr_info("SMP: IPI interrupt works -- AP took the level-6 mailbox IRQ\n");

	/*
	 * Interrupt test 2: does the AP take its own VIC clock as a REAL interrupt?
	 * The AP has installed our handler at the level-4 autovector, re-enabled the
	 * clock, and unmasked level 4.  No doorbell here -- the tick is autonomous;
	 * just wait for the handler's magic.
	 */
	if (!e17_wait_scratch(E17_SCR_VIC, E17_AP_VIC_IRQ_MAGIC, 3000)) {
		pr_err("SMP: VIC-clock INTERRUPT FAILED -- AP never took the level-4 tick IRQ\n");
		goto stop;
	}
	pr_info("SMP: VIC-clock interrupt works -- AP took the level-4 tick IRQ\n");

	/*
	 * Extended cross-CPU coherency test: ping-pong a changing value many times.
	 * Each round the boot CPU writes a fresh V (all bits vary round to round),
	 * IPIs the AP, and waits for the ack; the AP reads V and writes back ~V
	 * before acking.  The boot CPU then reads the long and checks it equals ~V.
	 * A stale/incoherent cache on either side would surface as a mismatch.
	 */
	{
		int n, failed = -1;
		bool timeout = false;
		u32 v = 0, exp = 0, got = 0;

		for (n = 0; n < E17_COH_ROUNDS; n++) {
			v = 0x9e3779b9u * (u32)(n + 1);	/* golden ratio: all bits churn */
			WRITE_ONCE(e17_ap_scratch[E17_SCR_COH], v);
			wmb();
			if (!e17_ping_secondary(cpu2con, 1000)) {
				failed = n;
				timeout = true;
				break;
			}
			rmb();
			got = READ_ONCE(e17_ap_scratch[E17_SCR_COH]);
			exp = ~v;
			if (got != exp) {
				failed = n;
				break;
			}
		}
		if (failed < 0)
			pr_info("SMP: cross-CPU coherency OK -- %d ping-pong rounds all matched\n",
				E17_COH_ROUNDS);
		else if (timeout)
			pr_err("SMP: coherency: AP did not ack at round %d (timeout)\n", failed);
		else
			pr_err("SMP: coherency FAIL at round %d: wrote 0x%08x expected 0x%08x got 0x%08x\n",
			       failed, v, exp, got);
		if (failed >= 0)
			goto stop;		/* park on any coherency failure */
	}

	/*
	 * The AP is running and about to enable its caches at kernel entry, so turn
	 * ON its hardware snoop bit NOW -- BEFORE the come-in below triggers the AP's
	 * MMU+D-cache enable.  Its /MI output is valid (it passed the handshake), so
	 * this satisfies the manual's "only snoop a running secondary" rule, and it
	 * closes the window where the AP could cache shared data with snoop off.
	 * (No-op unless CONFIG_E17_SMP_DCACHE; __cpu_up() also calls it, idempotent.)
	 */
	e17_smp_enable_snoop(cpu);

	/*
	 * All AP self-checks passed.  Stage 1 of kernel entry: signal "come in" by
	 * clearing the coherency slot to 0 and ringing the doorbell.  The AP acks,
	 * then ENABLES ITS MMU and waits for a second IPI.
	 */
	WRITE_ONCE(e17_ap_scratch[E17_SCR_COH], 0);
	wmb();
	if (!e17_ping_secondary(cpu2con, 1000)) {
		pr_err("SMP: AP did not ack the come-in sentinel\n");
		goto stop;
	}
	pr_info("SMP: AP acked come-in; it is enabling its MMU...\n");

	/*
	 * Stage 2: validate the AP's MMU.  Clear the vmalloc test slot, ring the
	 * doorbell; the AP (now with its MMU on) writes E17_AP_MMU_MAGIC to that
	 * kernel VIRTUAL address and acks.  Read it back through our own mapping: a
	 * match proves the secondary's page-table translation is correct.
	 */
	WRITE_ONCE(*e17_ap_mmu_test, 0);
	wmb();
	if (!e17_ping_secondary(cpu2con, 1000)) {
		pr_err("SMP: AP did not ack the MMU-validation IPI (stuck in/after MMU enable)\n");
		goto stop;
	}
	rmb();
	if (READ_ONCE(*e17_ap_mmu_test) != E17_AP_MMU_MAGIC) {
		pr_err("SMP: AP MMU FAILED -- kernel vaddr 0x%08lx = 0x%08x (expected 0x%08x)\n",
		       (unsigned long)e17_ap_mmu_test,
		       READ_ONCE(*e17_ap_mmu_test), E17_AP_MMU_MAGIC);
		goto stop;
	}
	pr_info("SMP: AP MMU OK -- secondary translated a kernel vaddr correctly\n");

	/*
	 * Stage 3: the AP has now jumped into secondary_start_kernel().  Poll its
	 * progress (e17_ap_progress, updated by the AP at each C milestone) with a
	 * tick-INDEPENDENT busy wait -- so neither a frozen jiffies nor a console
	 * stall can hang us here -- and report how far it gets.
	 */
	{
		u32 prog = 0, last = 0;
		int i;

		for (i = 0; i < 3000; i++) {		/* ~3s of mdelay(1) */
			rmb();
			prog = READ_ONCE(e17_ap_progress);
			if (prog != last) {
				pr_info("SMP: AP secondary_start_kernel: progress %u/%u\n",
					prog, E17_AP_PROG_IDLE);
				last = prog;
			}
			if (prog >= E17_AP_PROG_IDLE)
				break;
			mdelay(1);
		}
		if (prog >= E17_AP_PROG_IDLE) {
			pr_info("SMP: AP reached the idle loop -- letting cpuhp finish online\n");
			return 0;	/* __cpu_up() confirms via cpu_running */
		}
		pr_err("SMP: AP STALLED in secondary_start_kernel at progress %u/%u\n",
		       prog, E17_AP_PROG_IDLE);
	}

stop:
	/*
	 * Failure path: some pre-flight check or kernel-entry stage above failed, so the
	 * AP is stuck mid-bring-up (or looping in its stub) rather than in the kernel.
	 * Do NOT leave it running loose -- polling the bus and 7-seg, it races the boot
	 * CPU and wedges/crashes the boot.  Re-assert reset (SRESET=0) to PARK the
	 * secondary, and return an error so cpuhp rolls the online back cleanly (cpu1
	 * stays offline).  The success path above returns 0 without reaching here.
	 */
	writeb(0x00, cpu2con);			/* SRESET=0: hold the secondary in reset */
	pr_info("SMP: secondary parked (reset re-asserted); cpu%u left offline\n", cpu);
	return -EIO;
}

/* ------------------------------------------------------------------------- */
/* Snoop control (SNCR $FEC5E000, write-only -> software shadow).            */
/* ------------------------------------------------------------------------- */

static DEFINE_SPINLOCK(e17_sncr_lock);
static u8 e17_sncr_shadow;		/* mirrors the write-only SNCR */

void e17_smp_enable_snoop(unsigned int cpu)
{
	unsigned long flags;

	if (!IS_ENABLED(CONFIG_E17_SMP_DCACHE))
		return;			/* Tier 0: D-cache off, no snoop needed */

	spin_lock_irqsave(&e17_sncr_lock, flags);

	/*
	 * SC=01 ("supply dirty & sink") for the given CPU. The secondary's bits
	 * may be set ONLY once it is running (manual line 10545); __cpu_up()
	 * calls us for cpu 1 after the completion, i.e. after it is online.
	 */
	if (cpu == 0)
		e17_sncr_shadow |= E17_SNCR_PRI_SNOOP;
	else
		e17_sncr_shadow |= E17_SNCR_SEC_SNOOP;

	writeb(e17_sncr_shadow, (void __iomem *)E17_SNCR);

	spin_unlock_irqrestore(&e17_sncr_lock, flags);

	pr_info("E17 SMP: snoop enabled for cpu%u -> SNCR=0x%02x\n",
		cpu, e17_sncr_shadow);
}

/*
 * Disable a CPU's snoop bits.  The secondary's snoop MUST be turned off before
 * it is put back into reset (offline): once it stops running its /MI output is
 * invalid, and leaving SC!=00 for a non-running CPU corrupts snoop cycles on the
 * bus.  Call this on the offline path before re-asserting SRESET.
 */
void e17_smp_disable_snoop(unsigned int cpu)
{
	unsigned long flags;

	if (!IS_ENABLED(CONFIG_E17_SMP_DCACHE))
		return;

	spin_lock_irqsave(&e17_sncr_lock, flags);

	if (cpu == 0)
		e17_sncr_shadow &= ~E17_SNCR_PRI_SNOOP;
	else
		e17_sncr_shadow &= ~E17_SNCR_SEC_SNOOP;

	writeb(e17_sncr_shadow, (void __iomem *)E17_SNCR);

	spin_unlock_irqrestore(&e17_sncr_lock, flags);
}

/* ------------------------------------------------------------------------- */
/* Inter-processor interrupt: ring + acknowledge.                            */
/* ------------------------------------------------------------------------- */

void e17_send_ipi(const struct cpumask *mask, unsigned int ipi_msg)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		/* publish the message (set_bit implies a barrier) */
		e17_ipi_set_pending(cpu, ipi_msg);

		/*
		 * Primary -> secondary: ring the real CPU2CON mailbox doorbell.  Writing
		 * SIPL[2:0]=level with SRESET(bit5) kept set raises an autovectored
		 * level-6 IRQ on the secondary (QEMU e17-sysc models this; manual Table
		 * 50).  The secondary's e17_ipi_isr() edge-acks it with SICF=IACK_MBOX
		 * (which drops the level so it does not storm) and drains the bitmap.
		 *
		 * Secondary -> primary: ring the VIC068A ICMS module-switch doorbell
		 * (a clear->set edge on our module switch, ICFSR bit 0).  With ICMS0
		 * unmasked in ICMSICR the VIC raises the ICMS group interrupt on the
		 * primary at the IPL and vector we programmed (level 6, ICMSIVBR-based
		 * 0x40 -> IRQ_USER); e17_ipi_isr() there drops the bit (e17_ipi_ack_self)
		 * to deassert it and drains the bitmap.  (e17_ipi_poll() only carries
		 * IPIs in the CONFIG_E17_SMP_HW_IPI=n poll-only baseline.)
		 */
		if (IS_ENABLED(CONFIG_E17_SMP_HW_IPI)) {
			if (cpu == 1) {
				writeb(E17_CPU2CON_SRESET | E17_IPI_SEC_IPL,
				       (void __iomem *)E17_CPU2CON);
			} else {
				void __iomem *icfsr = (void __iomem *)E17_VIC_ICFSR;
				u8 v = readb(icfsr) & ~E17_ICMS_IPI_SW;

				writeb(v, icfsr);			/* ensure 0    */
				writeb(v | E17_ICMS_IPI_SW, icfsr);	/* 0 -> 1 edge */
			}
		}
	}
}

void e17_ipi_ack_self(void)
{
	if (smp_processor_id() == 1) {
		/* secondary: SICF=101 IACKs the mailbox IRQ, clears SIPL+MIPEND */
		writeb(E17_SICF_IACK_MBOX, (void __iomem *)E17_CPU2CON);
	} else {
		/* primary: drop our ICMS module-switch bit so the next edge is seen */
		void __iomem *icfsr = (void __iomem *)E17_VIC_ICFSR;

		writeb(readb(icfsr) & ~E17_ICMS_IPI_SW, icfsr);
	}
}

/*
 * Mailbox doorbell ISR.  Registered on IRQ_AUTO_6 and taken by the SECONDARY
 * when the primary rings the CPU2CON mailbox (e17_send_ipi).  e17_ipi_ack_self()
 * writes SICF=IACK_MBOX to drop the level-6 line before we drain, so the
 * level-triggered IRQ does not re-fire while we work.
 */
static irqreturn_t e17_ipi_isr(int irq, void *dev_id)
{
	unsigned int cpu = smp_processor_id();
	unsigned long ops;

	e17_ipi_ack_self();			/* edge-ack the hardware first */

	while ((ops = e17_ipi_take_pending(cpu)) != 0) {
		do {
			unsigned int msg = __ffs(ops);

			ops &= ~(1UL << msg);
			handle_IPI(msg);
		} while (ops);
	}
	return IRQ_HANDLED;
}

/*
 * Drain the local pending IPI bitmap.  This is the CONFIG_E17_SMP_HW_IPI=n
 * poll-only baseline: both directions rely on it, called from the idle loop and
 * the timer tick.  With HW_IPI both doorbells are real, hardware-confirmed IRQs
 * (CPU2CON mailbox pri->sec, VIC ICMS module switch sec->pri) drained by
 * e17_ipi_isr(), so this poll is not called at all -- idle halts on stop and the
 * tick backstop is compiled out.
 */
void e17_ipi_poll(void)
{
	unsigned int cpu = raw_smp_processor_id();
	unsigned long ops;

	/*
	 * Pure software-bitmap drain -- NO hardware doorbell ack.
	 *
	 * This is called from every idle iteration and every timer tick, so it must
	 * be cheap and must NOT touch I/O when there is nothing to do.  The old
	 * version called e17_ipi_isr(), which did an e17_ipi_ack_self() (a CPU2CON /
	 * ICFSR I/O write) on EVERY call -- from the AP's tight busy-idle loop that
	 * is a relentless I/O write stream that contends with the primary on the
	 * local bus.  In Tier-0 polling mode there is no hardware doorbell to ack
	 * anyway, so just xchg the pending word and dispatch.
	 */
	if (!e17_ipi_peek(cpu))		/* non-locked fast path: nothing pending */
		return;

	while ((ops = e17_ipi_take_pending(cpu)) != 0) {
		do {
			unsigned int msg = __ffs(ops);

			ops &= ~(1UL << msg);
			handle_IPI(msg);
		} while (ops);
	}
}

/* ------------------------------------------------------------------------- */
/* Init: register the cross-call, unmask the doorbells, request the IRQs.     */
/* ------------------------------------------------------------------------- */

void __init e17_smp_init_cpus(void)
{
	/*
	 * Mark both sockets possible. (setup_arch() calls the generic
	 * smp_init_cpus(), which reads the DT cpu@N nodes; this helper is here
	 * for the board to force cpu 1 possible even without a DT cpu@1 node.)
	 */
	set_cpu_possible(0, true);
	set_cpu_possible(1, true);
}

static int __init e17_smp_backend_init(void)
{
	if (num_possible_cpus() < 2)
		return 0;

	set_smp_cross_call(e17_send_ipi);

	/*
	 * Primary -> secondary IPIs are interrupt-driven via the CPU2CON mailbox:
	 * register the ISR on the level-6 autovector.  The interrupt is taken by the
	 * secondary (the only CPU the mailbox targets); the shared irq_desc is
	 * harmless on the primary, which never asserts autovector 6 (its devices are
	 * VIC-vectored, not autovectored).  IRQF_SHARED so it coexists cleanly.
	 *
	 * Secondary -> primary IPIs are interrupt-driven via the VIC068A ICMS
	 * module-switch doorbell (HW manual 3.19.2).  Register the handler on the
	 * ICMS0 IACK vector (0x40 -> IRQ_USER), then arm the hardware:
	 *   - ICMSIVBR := 0x40 so the ICMS0 IACK supplies vector 0x40 (must be
	 *     written after reset; reset value is $F0).
	 *   - ICMSICR: clear ICMS0's mask (bit 4) to enable it and set the group IPL
	 *     (bits 2-0) to level 6, leaving ICMS1-3 masked.
	 * With this in place both directions are interrupt-driven, so idle halts on
	 * stop and e17_ipi_poll() is not used (it is the HW_IPI=n baseline only).
	 */
	if (IS_ENABLED(CONFIG_E17_SMP_HW_IPI)) {
		void __iomem *icmsicr = (void __iomem *)E17_VIC_ICMSICR;
		void __iomem *icmsivbr = (void __iomem *)E17_VIC_ICMSIVBR;
		u8 icr;

		if (request_irq(IRQ_AUTO_6, e17_ipi_isr,
				IRQF_SHARED | IRQF_NO_THREAD,
				"ipi-mailbox", &e17_ipi_isr))
			pr_err("E17 SMP: failed to register mailbox IPI on IRQ_AUTO_6\n");

		if (request_irq(IRQ_USER, e17_ipi_isr, IRQF_NO_THREAD,
				"ipi-icms", &e17_ipi_poll))
			pr_err("E17 SMP: failed to register ICMS IPI on IRQ_USER\n");

		/* arm the ICMS0 doorbell: vector base, then unmask + set IPL */
		writeb(E17_ICMS_IPI_VEC, icmsivbr);
		icr = readb(icmsicr);
		icr &= ~(E17_ICMS0_MASK | 0x07);	/* enable ICMS0, clear IPL field */
		icr |= E17_IPI_PRI_LEVEL;		/* group IPL = level 6           */
		writeb(icr, icmsicr);
		pr_info("E17 SMP: IPI backend up (mbox IRQ pri->sec, ICMS IRQ sec->pri)\n");
	} else {
		pr_info("E17 SMP: IPI backend up (poll-only, both directions)\n");
	}
	return 0;
}
early_initcall(e17_smp_backend_init);

/*
 * Caches-ON cross-CPU coherency self-test (Tier-1 verification).
 *
 * The trampoline coherency test runs with the AP's caches OFF, so it proves the
 * shared-DRAM path but NOT that write-through + 68040 bus snooping (SNCR) keeps
 * the two ENABLED data caches coherent.  This runs once, late, with both CPUs
 * fully online and caching: the boot CPU writes a kernel word, has CPU1 invert
 * it via smp_call_function_single(), and checks the result.  It only passes if
 * CPU1's store is visible to CPU0's cache (snooping actually works).  A
 * supervisor (kernel .bss) word is used; if this PASSES but user space still
 * faults, the problem is the USER page cache mode, not snooping.
 *
 * NOTE: relies on smp_call_function_single() completing, i.e. on the IPI
 * doorbells working in both directions (mailbox pri->sec, ICMS sec->pri).  That
 * holds on real hardware and on the QEMU e17 model.
 */
static u32 *e17_coh_ptr;		/* word currently under test */

static void e17_coh_invert(void *unused)
{
	u32 v = READ_ONCE(*e17_coh_ptr);

	WRITE_ONCE(*e17_coh_ptr, ~v);
}

/* Ping-pong @word between the CPUs @rounds times; return the mismatch count. */
static unsigned int e17_coh_run(u32 *word, unsigned int rounds)
{
	unsigned int i, fails = 0;

	e17_coh_ptr = word;
	for (i = 0; i < rounds; i++) {
		u32 v = (0x11111111u * (i & 0xf)) ^ 0xa5a5a5a5u;

		WRITE_ONCE(*word, v);
		smp_call_function_single(1, e17_coh_invert, NULL, 1);
		if (READ_ONCE(*word) != (u32)~v)
			fails++;
	}
	return fails;
}

/*
 * The SMP self-tests (coherency / atomicity / concurrent) are OFF by default and
 * enabled with "e17_smptest" on the kernel command line.  They cost a few
 * seconds of boot and the copyback sub-test is an expected FAIL on this board
 * (copyback+snoop is incoherent -- we run write-through), so they are noise in
 * normal operation; keep them handy to re-validate the SMP/cache plumbing if
 * corruption ever reappears.
 */
static bool e17_smptest __initdata;
static int __init e17_smptest_setup(char *str)
{
	e17_smptest = true;
	return 1;
}
__setup("e17_smptest", e17_smptest_setup);

static u32 e17_coh_wt __cacheline_aligned;	/* supervisor .bss => write-through */

static int __init e17_coherency_selftest(void)
{
	unsigned int rounds = 4096, f_wt, f_cb = 0;
	unsigned long cbpage;

	if (!e17_smptest)
		return 0;
	if (num_online_cpus() < 2) {
		pr_info("E17 SMP: coherency self-test skipped (1 CPU online)\n");
		return 0;
	}

	pr_info("E17 SMP: cachemodes: mm_cachebits=0x%lx supervisor=0x%x pgtable=0x%x (0x20=copyback, 0x00=write-through)\n",
		mm_cachebits, m68k_supervisor_cachemode, m68k_pgtable_cachemode);

	/* (1) write-through word (this is how user pages SHOULD be mapped). */
	f_wt = e17_coh_run(&e17_coh_wt, rounds);

	/* (2) copyback word: remap a page copyback and repeat -- tells us whether
	 * snooping keeps COPYBACK data coherent, i.e. whether user pages MUST be
	 * write-through (as the HW manual states) or copyback would do too. */
	cbpage = __get_free_page(GFP_KERNEL);
	if (cbpage) {
		kernel_set_cachemode((void *)cbpage, PAGE_SIZE, IOMAP_FULL_CACHING);
		f_cb = e17_coh_run((u32 *)cbpage, rounds);
		kernel_set_cachemode((void *)cbpage, PAGE_SIZE, IOMAP_WRITETHROUGH);
		free_page(cbpage);
	}

	pr_info("E17 SMP: coherency (SNCR=0x%02x): write-through %s (%u/%u), copyback %s (%u/%u)\n",
		e17_sncr_shadow,
		f_wt ? "FAILED" : "OK", f_wt, rounds,
		cbpage ? (f_cb ? "FAILED" : "OK") : "n/a", f_cb, rounds);
	return 0;
}
late_initcall(e17_coherency_selftest);

/*
 * Cross-CPU atomicity self-test.  The SMP kernel relies on the 68040 CAS
 * instruction being atomic ACROSS CPUs (bus-locked): every spinlock, refcount
 * and atomic_t depends on it.  If the board's bus arbiter does not honour the
 * '040 /LOCK during CAS, two CPUs can read-modify-write the same location at
 * once and lose updates -- which silently corrupts kernel locks/refcounts and
 * shows up as *random* damage to whatever memory (e.g. a user page's mapping or
 * stack) the lost update was protecting.  Coherency being fine does NOT imply
 * atomicity.  Here both CPUs hammer one atomic_t; a short count means CAS is not
 * cross-CPU atomic.
 */
static atomic_t e17_atom;
#define E17_ATOM_ITERS	200000u

static void e17_atom_hammer(void *unused)
{
	unsigned int i;

	for (i = 0; i < E17_ATOM_ITERS; i++)
		atomic_inc(&e17_atom);
}

static int __init e17_atomicity_selftest(void)
{
	unsigned int expect, got;

	if (!e17_smptest)
		return 0;
	if (num_online_cpus() < 2) {
		pr_info("E17 SMP: atomicity self-test skipped (1 CPU online)\n");
		return 0;
	}

	atomic_set(&e17_atom, 0);
	/* run the hammer on BOTH CPUs at once (self + remote via IPI) */
	on_each_cpu(e17_atom_hammer, NULL, 1);

	expect = E17_ATOM_ITERS * num_online_cpus();
	got = atomic_read(&e17_atom);
	if (got != expect)
		pr_err("E17 SMP: CAS atomicity FAILED: %u increments, expected %u (lost %u) -- CAS is NOT cross-CPU atomic!\n",
		       got, expect, expect - got);
	else
		pr_info("E17 SMP: CAS atomicity OK (%u concurrent increments) -- CAS is cross-CPU atomic\n",
			got);
	return 0;
}
late_initcall(e17_atomicity_selftest);

/*
 * TRULY-CONCURRENT coherency stress test.
 *
 * The other self-tests miss the case that actually breaks this board: two CPUs
 * touching shared memory SIMULTANEOUSLY without a bus lock.  The sequential
 * coherency test ping-pongs (never simultaneous); the atomicity test uses CAS
 * which bus-locks (serialises).  Here two kthreads, pinned one per CPU, share a
 * single cache line (deliberate false sharing): each bumps its own monotonic
 * counter and reads the other's.  A correctly-coherent system NEVER sees the
 * other counter go backwards; a stale read under concurrent snoop load shows up
 * as a "backwards" event.  This is the pattern a spinlock release / lockless
 * read depends on, i.e. the thing that corrupts user memory and deadlocks the
 * box under load.
 */
static struct { volatile u32 seq[2]; } e17_cc __cacheline_aligned;
static atomic_t e17_cc_go;
static atomic_t e17_cc_anom;
static atomic_t e17_cc_done;

static int e17_cc_thread(void *arg)
{
	unsigned int me = (unsigned int)(unsigned long)arg;
	unsigned int other = me ^ 1;
	unsigned long endj;
	u32 last = 0, cur, mine = 0;
	unsigned int anom = 0;

	while (!atomic_read(&e17_cc_go))
		cpu_relax();

	endj = jiffies + msecs_to_jiffies(3000);
	while (time_before(jiffies, endj)) {
		WRITE_ONCE(e17_cc.seq[me], ++mine);
		cur = READ_ONCE(e17_cc.seq[other]);
		if (cur < last)		/* other's monotonic counter went backwards */
			anom++;		/* == a stale read == incoherent under load  */
		last = cur;
	}

	atomic_add(anom, &e17_cc_anom);
	atomic_inc(&e17_cc_done);
	return 0;
}

static int __init e17_concurrent_coherency_selftest(void)
{
	struct task_struct *t0, *t1;
	int i;

	if (!e17_smptest)
		return 0;
	if (num_online_cpus() < 2) {
		pr_info("E17 SMP: concurrent coherency test skipped (1 CPU online)\n");
		return 0;
	}

	atomic_set(&e17_cc_go, 0);
	atomic_set(&e17_cc_anom, 0);
	atomic_set(&e17_cc_done, 0);
	e17_cc.seq[0] = 0;
	e17_cc.seq[1] = 0;

	t0 = kthread_create(e17_cc_thread, (void *)0UL, "e17cc/0");
	t1 = kthread_create(e17_cc_thread, (void *)1UL, "e17cc/1");
	if (IS_ERR(t0) || IS_ERR(t1)) {
		pr_err("E17 SMP: concurrent coherency test: kthread_create failed\n");
		return 0;
	}
	kthread_bind(t0, 0);
	kthread_bind(t1, 1);
	wake_up_process(t0);
	wake_up_process(t1);

	atomic_set(&e17_cc_go, 1);		/* release both to run simultaneously */

	/* Bounded wait so a severe incoherency (go-flag never seen) can't hang boot. */
	for (i = 0; i < 100 && atomic_read(&e17_cc_done) < 2; i++)
		msleep(100);

	if (atomic_read(&e17_cc_done) < 2)
		pr_err("E17 SMP: CONCURRENT coherency test DID NOT FINISH (%d/2) -- a thread never saw the start flag (severe incoherency) or hung\n",
		       atomic_read(&e17_cc_done));
	else if (atomic_read(&e17_cc_anom))
		pr_err("E17 SMP: CONCURRENT coherency FAILED: %d stale/backwards reads under simultaneous load -- snoop is NOT coherent under concurrency\n",
		       atomic_read(&e17_cc_anom));
	else
		pr_info("E17 SMP: concurrent coherency OK (0 stale reads, 3s simultaneous load)\n");
	return 0;
}
late_initcall(e17_concurrent_coherency_selftest);

/*
 * Secondary -> primary IPI self-test (behind "e17_smptest").  Forces a
 * CPU1 -> CPU0 IPI via the VIC068A ICMS module-switch doorbell and checks the
 * primary's ipi-icms IRQ (IRQ_USER) actually incremented -- i.e. the interrupt
 * path delivered, not just the e17_ipi_poll() backstop.  This is the check that
 * tells real hardware apart from the poll fallback.
 */
static void e17_icms_noop(void *info) { }
static long e17_icms_ping(void *unused)
{
	/* Runs on CPU1: fire a CPU1->CPU0 IPI and wait for CPU0 to service it. */
	return smp_call_function_single(0, e17_icms_noop, NULL, 1);
}
static int __init e17_icms_selftest(void)
{
	unsigned int before, after;

	if (!e17_smptest)
		return 0;
	if (num_online_cpus() < 2) {
		pr_info("E17 SMP: ICMS self-test skipped (1 CPU online)\n");
		return 0;
	}

	before = kstat_irqs_cpu(IRQ_USER, 0);
	work_on_cpu(1, e17_icms_ping, NULL);	/* CPU1 -> CPU0 IPI */
	mdelay(50);				/* let the ICMS interrupt land on CPU0 */
	after = kstat_irqs_cpu(IRQ_USER, 0);

	pr_info("E17 SMP: ICMS self-test: CPU1->CPU0 IPI delivered; ipi-icms count %u->%u (%s)\n",
		before, after,
		after > before ? "INTERRUPT-driven"
			       : "POLL-only -- ICMS irq did NOT fire");
	return 0;
}
late_initcall(e17_icms_selftest);
