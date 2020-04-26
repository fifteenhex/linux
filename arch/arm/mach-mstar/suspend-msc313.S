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
	// 1. DDR enter self-refresh
	//wriu -w   0x101246,0xFFFE
	//wriu -w   0x101266,0xFFFF
	//wriu -w   0x101286,0xFFFF
	//wriu -w   0x1012A6,0xFFFF
	//wriu -w   0x101106,0xFFFF
	//wriu -w   0x101126,0xFFFF

	// miu-2
	ldr	r4, [r0, #0x10]
	ldr	r5, =0xFFFE
	str	r5, [r4, #0x46 << 1]
	ldr	r5, =0xFFFF
	str	r5, [r4, #0x66 << 1]
	str	r5, [r4, #0x86 << 1]
	str	r5, [r4, #0xA6 << 1]
	// miu-1
	ldr	r4, [r0, #0xc]
	str	r5, [r4, #0x06 << 1]
	str	r5, [r4, #0x26 << 1]

	//wriu -w 0x101218,0x0400  //reg_mrx
	//wriu -w 0x101200,0x002f  //Bit[05]reg_auto_ref_off
	//wriu -w 0x101200,0x052e  //trig precharge all
	//wriu -w 0x101200,0x002e
	//wriu -w 0x101200,0x032e
	//wriu -w 0x101200,0x002e
	//wriu -w 0x101206,0x1430
	//wriu -w 0x101246,0xFFFF
	//wriu -w 0x101200,0x202e
	// miu-2
	ldr	r4, [r0, #0x10]
	ldr	r5, =0x0400
	str	r5, [r4, #0x18 << 1]
	ldr	r5, =0x002F
	str	r5, [r4, #0x00 << 1]
	ldr	r5, =0x052E
	str	r5, [r4, #0x00 << 1]
	ldr	r5, =0x002E
	str	r5, [r4, #0x00 << 1]
	ldr	r5, =0x032E
	str	r5, [r4, #0x00 << 1]
	ldr	r5, =0x002E
	str	r5, [r4, #0x00 << 1]
	ldr	r5, =0x1430
	str	r5, [r4, #0x06 << 1]
	ldr	r5, =0xFFFF
	str	r5, [r4, #0x46 << 1]
	ldr	r5, =0x202E
	str	r5, [r4, #0x00 << 1]

	// load up pmsleep
	ldr	r4, [r0, #0x0]

	// set the wake up source
	ldr   r5, [r4, #0x10 << 1]
	bic   r5, r5, #0x16      //[1]:SAR, [2]:WOL, [4]:RTC
	str   r5, [r4, #0x10 << 1]

	// 4. PM sleep mode
	// clk gen
	//wriu -w 0x0e40 0x0000  //reg_ckg_mcu, reg_ckg_spi to xtal
	ldr	r4, [r0, #0x14]
	ldr	r5, [r4]
	//ldr	r5, =0
	bic	r5, r5, #0x1 << 7
	bic	r5, r5, #0x1 << 14
	str	r5, [r4]

    // something new
    //wriu    0x3c24 0x30    //reg_pwoff_rst0_en, reg_pwoff_rst1_en
//    ldr   r0, =0x30
//    strb  r0, [r2, #0x24 << 1]

	// pmsleep
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
	// pm gpio
	ldr	r4, [r0, #0x4]
	ldr	r5, =0x10
	strb	r5, [r4, #0x08 << 1]

	// spin
	nop
	nop
	nop
	nop
ENDPROC(msc313_suspend_imi)
.ltorg
