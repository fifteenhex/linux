/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * arch/m68k/kernel/smp_secondary.S
 *
 * Application-processor (secondary CPU) entry code for the ELTEC EUROCOM-17-5xx.
 *
 * Two-page, self-contained bring-up model.  The boot CPU allocates two
 * CONTIGUOUS pages and copies this blob to the start of them:
 *
 *   page0 + 0x000 : ap_vectors  - a 256-entry (1 KiB) exception vector table.
 *                   The boot CPU fills every slot with the physical address of
 *                   ap_exc_handler (below), so ANY exception the AP takes lands
 *                   in our own handler -- never RMON, never a stale/garbage slot.
 *   page0 + 0x400 : ap_entry    - the reset entry point (PC planted in DRAM[4]).
 *   page0 + ...   : ap_exc_handler - the shared exception handler.
 *   page1 (top)   : the pre-built pop-stack (SSP planted in DRAM[0]).
 *
 * The AP comes out of the CPU2CON release running PHYSICAL, MMU off, on that
 * stack.  It disables its caches, takes over VBR with our table immediately, and
 * (MILESTONE) rolls 0..F on the POST 7-seg forever.  If it ever faults, the
 * handler shows 'E' alternating with the vector number instead -- so a fault is
 * visibly distinct from the healthy animation, and we can read WHICH vector.
 *
 * Everything here is position-independent (PC-relative), so it runs correctly at
 * whatever physical address the pages landed on.
 */

#include <asm/smp_e17.h>

/*
 * Temporary instruction transparent-translation covering 0x00000000-0x0FFFFFFF
 * (DRAM/VRAM), identity, cache-inhibited: base=0x00 mask=0x0F E=1 S=match-both
 * CM=10.  Keeps the physical PC mapped across the movec %tc that enables the MMU.
 */
#define E17_TMP_ITT	0x000FC040

	.text

	.balign	4
	.globl	ap_blob_start
ap_blob_start:

/* 256 exception vectors (1 KiB).  Zero here; the boot CPU fills each entry with
 * the physical address of ap_exc_handler before releasing the AP. */
	.globl	ap_vectors
ap_vectors:
	.space	1024

/*
 * ap_entry - reset entry point.  PC (DRAM[4]) points here; SSP (DRAM[0]) already
 * points at the pre-built pop-stack.
 */
	.balign	4
	.globl	ap_entry
	.type	ap_entry,@function
ap_entry:
	/*
	 * Take over exception handling AS EARLY AS POSSIBLE.  The lea (a generic
	 * instruction, so it also pins GAS's arch for the following ".chip 68040")
	 * computes the physical base of our vector table PC-relatively; load it into
	 * VBR before anything that could fault.  Only the single first fetch (this
	 * lea) is still on whatever VBR the release left us.
	 */
	lea	ap_vectors(%pc),%a0		| a0 = phys base of our vector table
	.chip	68040
	movec	%a0,%vbr			| VBR -> our table: our handler catches all

	/* Disable + invalidate both caches (undefined power-on state at reset). */
	moveq	#0,%d0
	movec	%d0,%cacr			| caches OFF
	nop
	cinva	%bc				| discard garbage I/D lines
	nop
	.chip	68k

	/*
	 * Prove the boot CPU's pre-built stack: pop the E17_AP_NPOP kernel-entry
	 * values (discarded for now), then pop ONE more longword -- the physical
	 * address of a shared scratch long -- into %a3.  A wrong SSP would fault here
	 * -> our handler.
	 */
	moveq	#E17_AP_NPOP-1,%d3
1:	movel	%sp@+,%d0
	dbra	%d3,1b
	movel	%sp@+,%a3			| %a3 = &shared scratch long (physical)

	/*
	 * ===== FPU presence probe =====
	 * Done here, while we still own the private vector table, so hooking a vector
	 * can't race CPU0's (shared) kernel vectors.  Default the shared FPU slot to
	 * PRESENT, point the line-F vector (11) at ap_fpu_none, save SSP, and execute
	 * a harmless fnop.  If this CPU has no FPU the fnop takes a line-F exception
	 * into ap_fpu_none, which rewrites the slot to ABSENT and returns here via the
	 * saved SSP (it does NOT retry the fnop).  We then restore vector 11 to
	 * ap_exc_handler and continue.  The boot CPU reads scratch[3] and reports it.
	 * (%a3 = scratch base; %a5/%a4 not set up yet, so no display here.)
	 */
	movel	#E17_AP_FPU_PRESENT,%a3@(12)	| optimistic default: FPU present
	.chip	68040
	movec	%vbr,%a0			| a0 = our vector table base
	.chip	68k
	lea	ap_fpu_none(%pc),%a1
	movel	%a1,%a0@(11*4)			| vectors[11] (line-F) = ap_fpu_none
	movel	%sp,%a6				| a6 = SSP to restore if the probe traps
	.chip	68040
	fnop					| line-F trap -> ap_fpu_none if no FPU
	nop
	.chip	68k
.Lfpu_done:
	.chip	68040
	movec	%vbr,%a0
	.chip	68k
	lea	ap_exc_handler(%pc),%a1
	movel	%a1,%a0@(11*4)			| restore vectors[11] = ap_exc_handler

	/*
	 * Bring-up handshake, all by POLLING (interrupts stay MASKED, SR = IPL7 from
	 * reset -- we never enable them).  Display progresses 0 -> 1 -> 2 -> 3:
	 *   0 : init done, waiting for the boot CPU's first mailbox IPI
	 *   1 : IPI #1 seen + acked
	 *   2 : our own VIC-clock ticked (we enabled it and saw VICIRQ) -> acked
	 *   3 : IPI #2 seen + acked
	 * If the VIC clock never ticks, we stick at 1 (and the boot CPU's 2nd-IPI ack
	 * times out) -- that is how the boot CPU learns whether the secondary can see
	 * its own clock source.
	 */
	movel	#0xfec30000,%a5			| POST 7-seg
	movel	#0xfec58000,%a4			| CPU2CON
	moveb	#0,%a5@				| show 0 (waiting for IPI #1)

	/* --- wait for IPI #1 from the boot CPU, ack it --- */
.Lipi1:	moveb	%a4@,%d0
	andb	#E17_CPU2CON_MIPEND,%d0		| mailbox IPI pending? (0x80)
	beq	.Lipi1
	moveb	#E17_SICF_IACK_MBOX,%a4@	| ack IPI #1 (SICF=0x05)
	moveb	#1,%a5@				| show 1

	/* --- check our own VIC clock: enable it, wait one tick, ack it --- */
	moveb	#E17_SICF_VICCLK_EN,%a4@	| enable VIC-clock delivery (SICF=0x01)
.Lvic:	moveb	%a4@,%d0
	andb	#E17_CPU2CON_VICIRQ,%d0		| VIC-clock tick pending? (0x20)
	beq	.Lvic				| not yet -> keep polling (stuck if broken)
	moveb	#E17_SICF_IACK_VIC,%a4@		| ack the VIC-clock tick (SICF=0x04)
	moveb	#2,%a5@				| show 2 (own clock source works)

	/* --- wait for IPI #2, ack it --- */
.Lipi2:	moveb	%a4@,%d0
	andb	#E17_CPU2CON_MIPEND,%d0
	beq	.Lipi2
	moveb	#E17_SICF_IACK_MBOX,%a4@	| ack IPI #2
	moveb	#3,%a5@				| show 3

	/*
	 * ===== Test: mailbox IPI as a REAL interrupt (level 6) =====
	 * The boot CPU installed ap_ipi_handler at vector 30 (level-6 autovector).
	 * Drop the IPL mask to 5 so ONLY level 6 (the IPI) is taken -- level 4 (the
	 * VIC clock) stays masked so it can't interfere here -- and spin until the
	 * handler publishes the IPI magic.  Then re-mask.
	 */
	movew	#0x2500,%sr			| supervisor, IPL 5 (allow level 6 only)
.Lirq1:	cmpl	#E17_AP_IPI_IRQ_MAGIC,%a3@(4)	| scratch[1] set by ap_ipi_handler?
	bne	.Lirq1				| wait for the IPI handler to fire
	movew	#0x2700,%sr			| mask all again
	moveb	#4,%a5@				| show 4

	/*
	 * ===== Test: VIC clock as a REAL interrupt (level 4) =====
	 * ap_vic_handler is installed at vector 28.  Make sure the clock is running,
	 * drop the IPL mask to 3 so level 4 is taken, and spin until the handler
	 * publishes the VIC magic.  Then re-mask and stop the clock (the coherency
	 * loop below runs with interrupts masked and polls instead).
	 */
	moveb	#E17_SICF_VICCLK_EN,%a4@		| ensure the VIC clock is ticking
	movew	#0x2300,%sr			| supervisor, IPL 3 (allow level 4)
.Lirq2:	cmpl	#E17_AP_VIC_IRQ_MAGIC,%a3@(8)	| scratch[2] set by ap_vic_handler?
	bne	.Lirq2				| wait for the VIC handler to fire
	movew	#0x2700,%sr			| mask all again
	moveb	#E17_SICF_VICCLK_DIS,%a4@	| stop the clock
	moveb	#5,%a5@				| show 5

	/*
	 * Cross-CPU coherency ping-pong (runs forever, one round per IPI).  Each
	 * round: the boot CPU has written a fresh value V into the shared long (%a3)
	 * and rung the mailbox; we read V, write its inverse ~V back, then ack.  The
	 * boot CPU reads ~V and checks it.  We read V BEFORE acking and write ~V
	 * BEFORE acking, so the ack means "the inverse is in memory".  The 7-seg
	 * rolls 0..F with the round count so the loop is visibly alive.
	 */
	moveq	#0,%d1				| round counter (for the display)
.Lcoh:	moveb	%a4@,%d0
	andb	#E17_CPU2CON_MIPEND,%d0		| mailbox IPI for this round?
	beq	.Lcoh
	movel	%a3@,%d0			| V = *scratch (boot CPU's value)
	beq	.Lentry				| V == 0 => sentinel: go enter the kernel
	notl	%d0				| ~V
	movel	%d0,%a3@			| *scratch = ~V
	nop					| settle the store before the ack
	moveb	#E17_SICF_IACK_MBOX,%a4@	| ack this round
	addql	#1,%d1				| advance the visible round count
	movel	%d1,%d0
	andl	#0x0f,%d0
	moveb	%d0,%a5@			| roll 0..F
	jra	.Lcoh

	/*
	 * ===== Kernel entry, stage 1: enable the MMU, then VALIDATE it =====
	 * The boot CPU rang the doorbell with the coherency slot cleared to 0 as a
	 * "you passed, come in" signal.  Ack it, then enable the MMU exactly like the
	 * old trampoline (temporary identity ITT0 across the movec %tc so the physical
	 * PC survives; the page is lowmem so the kernel page tables also map it 1:1).
	 *
	 * Rather than immediately jumping into C, we first PROVE the MMU works: wait
	 * for another IPI, then write E17_AP_MMU_MAGIC to a kernel VIRTUAL address the
	 * boot CPU handed us (desc[12]) -- a store that only lands in the right place
	 * if our page-table translation is correct -- and ack.  The boot CPU reads it
	 * back.  Only once that passes do we (in a later step) do the real jump into
	 * secondary_start_kernel.  VBR stays our test table so any fault shows 'E'.
	 * 7-seg: 6 = entry asm, 7 = about to enable MMU, 8 = MMU on/awaiting IPI,
	 * 9 = wrote through the MMU.
	 */
.Lentry:
	moveb	#E17_SICF_IACK_MBOX,%a4@		| ack the come-in sentinel
	moveb	#6,%a5@				| 6: kernel-entry asm running
	movec	%vbr,%a0			| a0 = page0 base (VBR still our table)
	lea	%a0@(E17_AP_DESC_OFF),%a0	| a0 = kernel-entry descriptor
	movel	%a0@(48),%a6			| a6 = kernel VIRTUAL test address (desc[12])
	.chip	68040
	movel	%a0@(0),%d3			| d3 = cacr
	movel	%a0@(4),%d1			| d1 = tc (E-bit set)
	movel	%a0@(16),%d2			| d2 = real itt0 (restored post-enable)
	movel	%a0@(8),%d0
	movec	%d0,%srp
	movel	%a0@(12),%d0
	movec	%d0,%urp
	movel	%a0@(20),%d0
	movec	%d0,%itt1
	movel	%a0@(24),%d0
	movec	%d0,%dtt0
	movel	%a0@(28),%d0
	movec	%d0,%dtt1
	movel	#E17_TMP_ITT,%d0
	movec	%d0,%itt0			| temp identity ITT0 for the transition
	nop
	cinva	%bc
	movec	%d3,%cacr
	moveb	#7,%a5@				| 7: about to enable the MMU
	nop
	movec	%d1,%tc				| ENABLE MMU
	nop
	movec	%d2,%itt0			| restore the real ITT0
	nop
	cinva	%ic
	nop
	.chip	68k
	moveb	#8,%a5@				| 8: MMU on; waiting for the validation IPI

	/* wait for the validation IPI, then write the magic through our MMU + ack */
.Lmmuw:	moveb	%a4@,%d0
	andb	#E17_CPU2CON_MIPEND,%d0
	beq	.Lmmuw
	movel	#E17_AP_MMU_MAGIC,%a6@		| store via the kernel VIRTUAL address
	nop
	moveb	#E17_SICF_IACK_MBOX,%a4@		| ack the validation IPI
	moveb	#9,%a5@				| 9: wrote the magic through the MMU

	/*
	 * ===== Kernel entry, stage 2: jump into secondary_start_kernel =====
	 * MMU is on and proven.  Re-read the descriptor (a0 still points at it, and
	 * page0 is identity-mapped) for the kernel VBR, stack, current and entry, take
	 * VBR to the kernel's table, switch to the kernel stack, set current (%a2) and
	 * jump.  From here secondary_start_kernel drives the 7-seg (b,6,7,a,e,f) and
	 * e17_ap_progress, which the boot CPU polls.
	 */
	.chip	68040
	movel	%a0@(32),%d0
	movec	%d0,%vbr				| VBR -> kernel vector table
	.chip	68k
	movel	%a0@(36),%sp			| kernel stack (stack_virt)
	movel	%a0@(40),%a2			| current (idle task)
	movel	%a0@(44),%a1			| secondary_start_kernel
	jmp	%a1@				| -> C (a2 = current); never returns
	.size	ap_entry,.-ap_entry

/*
 * ap_exc_handler - every AP exception vectors here (VBR -> ap_vectors -> here).
 * Show 'E' alternating with the exception's vector number on the 7-seg forever,
 * so a fault is visibly distinct from the 0..F animation and we can read the
 * vector (2=bus, 3=addr, 4=illegal, 8=priv, a=lineA, b=lineF, ...).
 * SR/PC at frame+0/+2; the format/vector word at frame+6 is common to all
 * 680x0 frame formats.
 */
	.balign	4
	.globl	ap_exc_handler
	.type	ap_exc_handler,@function
ap_exc_handler:
	movew	%sp@(6),%d7			| format/vector word
	andl	#0x0fff,%d7			| -> vector offset (bytes)
	lsrl	#2,%d7				| -> vector number
	andl	#0x0f,%d7			| low nibble for display
	movel	#0xfec30000,%a5
4:	moveb	#0x0e,%a5@			| 'E' = the AP took an exception
	movel	#0x00080000,%d2
5:	subql	#1,%d2
	jne	5b
	moveb	%d7,%a5@			| the vector number
	movel	#0x00080000,%d2
6:	subql	#1,%d2
	jne	6b
	jra	4b
	.size	ap_exc_handler,.-ap_exc_handler

/*
 * ap_ipi_handler / ap_vic_handler - real interrupt handlers for the two IRQ
 * tests, installed by the boot CPU at the level-6 and level-4 autovectors.  Each
 * publishes its magic to the shared scratch long (%a3, set by ap_entry), acks
 * the source in CPU2CON (%a4), and returns.  They touch no data/address
 * registers, so no save/restore is needed.
 */
/*
 * ap_fpu_none - line-F (vector 11) handler used ONLY during the FPU probe.
 * Reached when the probe's fnop traps because this CPU has no FPU.  Rewrite the
 * shared FPU slot to ABSENT, discard the exception frame by restoring the SSP we
 * saved before the fnop (avoids depending on the frame size), and rejoin the
 * trampoline WITHOUT retrying the fnop.  (%a3 = scratch base, %a6 = saved SSP;
 * exceptions do not clobber the general registers, so both are still valid.)
 */
	.balign	4
	.type	ap_fpu_none,@function
ap_fpu_none:
	movel	#E17_AP_FPU_ABSENT,%a3@(12)	| no FPU on this CPU
	movel	%a6,%sp				| pop the exception frame (restore SSP)
	jra	.Lfpu_done			| rejoin; do not re-execute the fnop
	.size	ap_fpu_none,.-ap_fpu_none

	.balign	4
	.globl	ap_ipi_handler
	.type	ap_ipi_handler,@function
ap_ipi_handler:
	movel	#E17_AP_IPI_IRQ_MAGIC,%a3@(4)	| scratch[1]: level-6 mailbox IRQ fired
	moveb	#E17_SICF_IACK_MBOX,%a4@		| ack the mailbox
	rte
	.size	ap_ipi_handler,.-ap_ipi_handler

	.balign	4
	.globl	ap_vic_handler
	.type	ap_vic_handler,@function
ap_vic_handler:
	movel	#E17_AP_VIC_IRQ_MAGIC,%a3@(8)	| scratch[2]: level-4 VIC-clock IRQ fired
	moveb	#E17_SICF_IACK_VIC,%a4@		| ack the VIC clock
	rte
	.size	ap_vic_handler,.-ap_vic_handler

	.globl	ap_blob_end
ap_blob_end:


/*
 * e17_park_secondary() - park the calling CPU with interrupts masked.
 * Used by the IPI_CPU_STOP handler and offline.
 */
	.balign	4
	.globl	e17_park_secondary
	.type	e17_park_secondary,@function
e17_park_secondary:
	stop	#0x2700				| supervisor, IPL7, halt
	jra	e17_park_secondary		| defensive: re-park on spurious wake
	.size	e17_park_secondary,.-e17_park_secondary

	.chip	68k
