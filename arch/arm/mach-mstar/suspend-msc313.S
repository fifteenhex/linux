/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>

.align 3
.globl infinity_suspend_imi

.macro delay
	ldr   r5,=0x10000
	1:
	subs  r5, #1
	bne   1b
.endm

.macro setbank infooffset
	ldr	r4, [r0, #\infooffset]
.endm

.macro setbyte offset value
	ldr	r5, =\value
	strb	r5, [r4, #\offset << 1]
.endm

.macro setword offset value
	ldr	r5, =\value
	str	r5, [r4, #\offset << 1]
.endm

.macro setbits offset bits
	ldr	r5, [r4, #\offset << 1]
	orr	r5, r5, #\bits
	str	r5, [r4, #\offset << 1]
.endm

.macro clearbits offset bits
	ldr   r5, [r4, #\offset << 1]
	bic   r5, r5, #\bits
	str   r5, [r4, #\offset << 1]
.endm

//r0 should be a pointer to a struct mstar_pm_info
//r4 is used to track the base of the block we're poking
//r5 is used for the value that is being loaded

//This must run in SRAM
ENTRY(msc313_suspend_imi)
	// First block all access except one in group 0, cpu?
	// miu-2
	setbank 0x10
	setword 0x46 0xfffe
	setword 0x66 0xffff
	setword 0x86 0xffff
	setword 0xa6 0xffff
	// miu-1
	setbank	0x0c
	setword	0x00 0xffff
	setword 0x26 0xffff
	delay

	// miu-2
	setbank 0x10
	.macro miu_clear_single_cmd
		clearbits 0x00 0x0f00 // clear command issue and rasz, casz, wez bits
	.endm
	setword 0x18 0x0400 // single command mode register
	setword 0x00 0x002F // initial setting?
	setword 0x00 0x052E // issue command, casz ?
	miu_clear_single_cmd
	setbits	0x0 0x0300  // issue command, raz
	miu_clear_single_cmd
	setword 0x06 0x1430 // tref, tcke timing, cke always on?
	delay
	setword 0x46 0xFFFF // mask all requests
	setbits 0x00 0x2000 // enter self refresh
	delay

	// Now that the DDR is doing it's own thing disable
	// power everything down to save energy

    	// miu-2
    	setbank 0x10
    	setbits 0x04 0xf000 // mclk delay etc
	// miu-0
	setbank 0x8
	setbits 0x00 0x0018 // set gpio mode, output highz
	setword 0x54 0xc070 // unknown
	setword 0x08 0x0000 // clear rx en
	delay

	// Now prepare our wake up source
	setbank	0x0
	clearbits 0x10 0x16 //[1]:SAR, [2]:WOL, [4]:RTC

	// this should be handled in the clkgen gate driver
	// 4. PM sleep mode
	// clk gen
	//wriu -w 0x0e40 0x0000  //reg_ckg_mcu, reg_ckg_spi to xtal
	setbank	0x14
	ldr	r5, [r4]
	//ldr	r5, =0
	bic	r5, r5, #0x1 << 7
	bic	r5, r5, #0x1 << 14
	str	r5, [r4]

   	// something new
	//wriu    0x3c24 0x30    //reg_pwoff_rst0_en, reg_pwoff_rst1_en
	//    ldr   r0, =0x30
	//    strb  r0, [r2, #0x24 << 1]

	// Unlock the power down stuff
	setbank	0x0
	setbyte 0x38 0x0c
	setword 0x24 0xbabe
	setbyte 0x6e 0xa5
	delay

	// Finally set pm_gpio4 to turn the lights off
	setbank 0x4
	setbyte 0x08 0x10

	// spin
	b .
ENDPROC(msc313_suspend_imi)
.ltorg
