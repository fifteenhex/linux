/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Suspend to RAM for the SSD20xD, run from the on chip SRAM.
 *
 * The sequence is the vendor's: block the MIU clients, precharge and
 * refresh the DRAM by hand and put it into self refresh, power down the
 * DRAM pads and PLLs, run the CPU from the crystal and wait for an
 * interrupt. The wake interrupt does not resume anything: it takes us
 * out of WFI and we reset the SoC. The boot ROM and the first stage
 * loader then bring the DRAM controller back and jump to the resume
 * entry the kernel left in SRAM (see soc/mstar/str.h).
 *
 * r0 is a struct mstar_pm_info with the mapped register banks.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>

#define PMINFO_PMSLEEP		0x0
#define PMINFO_PMGPIO		0x4
#define PMINFO_MIU_ANA		0x8
#define PMINFO_MIU_DIG		0xc
#define PMINFO_MIU_ARB		0x10
#define PMINFO_PMUART		0x14
#define PMINFO_BANK1020		0x18
#define PMINFO_CPUPLL		0x1c
#define PMINFO_MPLL		0x20
#define PMINFO_FLAGS		0x24
#define PM_FLAG_KEEP_CPU	1
#define PM_FLAG_KEEP_MPLL	2

.macro delay ticks=0x10000
	ldr	r5, =\ticks
1:	subs	r5, #1
	bne	1b
.endm

/* read-modify-write a byte of a register bank: base r4, offset, clear, set */
.macro rmwb off, clr, set
	ldrb	r5, [r4, #\off]
	.if \clr
	bic	r5, r5, #\clr
	.endif
	.if \set
	orr	r5, r5, #\set
	.endif
	strb	r5, [r4, #\off]
.endm

.macro seth off, val
	ldr	r5, =\val
	add	r6, r4, #\off
	strh	r5, [r6]
.endm

/* progress letters on the PM UART, r7 is its base */
.macro putch ch
	mov	r5, #\ch
	str	r5, [r7]
	ldr	r5, =0x2000
2:	subs	r5, #1
	bne	2b
.endm

/* make sure a bank is in the TLB before the DRAM goes away */
.macro touch field
	ldr	r4, [r8, #\field]
	ldr	r5, [r4]
.endm

	.text
	.align 3
ENTRY(ssd20xd_suspend_imi)
	stmfd	sp!, {r4 - r8, lr}
	mov	r8, r0
	ldr	r7, [r8, #PMINFO_PMUART]
	putch	'S'

	touch	PMINFO_PMSLEEP
	touch	PMINFO_MIU_ANA
	touch	PMINFO_MIU_DIG
	touch	PMINFO_MIU_ARB
	touch	PMINFO_BANK1020
	touch	PMINFO_CPUPLL
	touch	PMINFO_MPLL

	/* stop every client but one of group 0 */
	ldr	r4, [r8, #PMINFO_MIU_DIG]
	seth	0x1c0, 0x0000
	seth	0x8c, 0xfffe
	seth	0xcc, 0xffff
	seth	0x10c, 0xffff
	seth	0x14c, 0xffff
	ldr	r4, [r8, #PMINFO_MIU_ARB]
	seth	0x1cc, 0x00fe
	delay

	putch	'b'
	/* precharge all, then one refresh, as single commands */
	ldr	r4, [r8, #PMINFO_MIU_DIG]
	seth	0x30, 0x0400
	rmwb	0, 0x30, 0x20
	rmwb	1, 0x1e, 0x04
	rmwb	1, 0x01, 0
	rmwb	1, 0, 0x01
	rmwb	1, 0x01, 0
	rmwb	1, 0x1e, 0x02
	rmwb	1, 0x01, 0
	rmwb	1, 0, 0x01
	rmwb	1, 0x01, 0
	delay

	putch	'p'
	/* self refresh */
	seth	0, 0x202e
	delay
	putch	'r'

	/* DRAM pads off: cke, address, dq and clock drivers disabled */
	rmwb	5, 0, 0xf0

	/* DRAM PHY analog down */
	ldr	r4, [r8, #PMINFO_MIU_ANA]
	rmwb	0x0, 0, 0x08
	rmwb	0x0, 0, 0x10
	rmwb	0x110, 0x1f, 0
	rmwb	0x115, 0, 0x02
	rmwb	0xa8, 0, 0x70
	rmwb	0x10, 0x3f, 0

	putch	'a'
	ldr	r5, [r8, #PMINFO_FLAGS]
	tst	r5, #PM_FLAG_KEEP_CPU
	bne	7f
	/* CPU on the crystal, CPU PLL off */
	ldr	r4, [r8, #PMINFO_BANK1020]
	mov	r5, #0
	strb	r5, [r4, #0x1f0]
	ldr	r4, [r8, #PMINFO_CPUPLL]
	mov	r5, #1
	strb	r5, [r4, #0x45]

7:
	ldr	r5, [r8, #PMINFO_FLAGS]
	tst	r5, #PM_FLAG_KEEP_MPLL
	bne	8f
	putch	'c'
	/* MPLL off, the console goes with it */
	ldr	r4, [r8, #PMINFO_MPLL]
	mov	r5, #0x10
	strb	r5, [r4, #0x05]
	mov	r5, #0
	strh	r5, [r4, #0x08]
	strh	r5, [r4, #0x10]
	strb	r5, [r4, #0x11]

8:
	/*
	 * The vendor code also powers the DRAM PLL down here (bits 15:14 of
	 * DDFSET_H). With that the chip never comes back from WFI on a wake
	 * interrupt, so it stays on.
	 */
	dsb
	wfi

	/* something wants us back: reset the SoC, the SRAM and the DRAM keep what they hold */
	ldr	r4, [r8, #PMINFO_PMSLEEP]
	mov	r5, #0x79
	strh	r5, [r4, #0xb8]
1:	b	1b
ENDPROC(ssd20xd_suspend_imi)
	.ltorg
