/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>

.align 3
.globl infinity_suspend_imi

//r0 should be a pointer to a struct mstar_pm_info
//r4 is used to track the base of the block we're poking
//r5 is used for the value that is being loaded

//This must run in SRAM
ENTRY(msc313_suspend_imi)
	// load up pmsleep
	ldr	r4, [r0, #0x0]

	ldr	r5, =0x0c
	strb	r5, [r4, #0x38 << 1]

	ldr   r5, =0xBABE
	str   r5, [r4, #0x24 << 1]
	ldr   r5, =0xA5
	strb  r5, [r4, #0x6E << 1]

	// from vendor code
	nop
	nop
	nop
	nop

	//wriu    0x0f08 0x10    //set PM_GPIO_4 oen=0, out=0
	ldr	r4, [r0, #0x4]
	ldr	r5, =0x10
	strb	r5, [r4, #0x08 << 1]

	// spin
	b .
ENDPROC(msc313_suspend_imi)
.ltorg
