/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>

.global _start
.global _ramvec
.global _dtblob
.global _is68k

.data

/*****************************************************************************
 * Stashes for bootloader parameters
 *****************************************************************************/
_ramvec:
	.long	0
_dtblob:
	.long	0
_is68k:
	.word	0

nullcatch:
	trap #1
	jmp .

__HEAD

/*****************************************************************************
 * Entry point, where all begins!
 *****************************************************************************/

#define MC68EZ328_UART			0xfffff900
#define MC68EZ328_UART_USTCNT	0x0
#define MC68EZ328_UART_USTCNT_87	(1 << 8)
#define MC68EZ328_UART_USTCNT_TXEN	(1 << 13)
#define MC68EZ328_UART_USTCNT_UEN	(1 << 15)
#define MC68EZ328_UART_UBAUD	0x2
#define MC68EZ328_UART_TXFLAGS	0x6
#define MC68EZ328_UART_TXDATA	0x7

.macro __putch val
1:
	//and.w	#(1 << 13),(MC68EZ328_UART + MC68EZ328_UART_TXFLAGS)
	move.w	(MC68EZ328_UART + MC68EZ328_UART_TXFLAGS),%d0
	and.w	#(1 << 13),%d0
	beq		1b
	move.b	#\val,(MC68EZ328_UART + MC68EZ328_UART_TXDATA)
.endm

.macro _putch val
	__putch \val
	__putch '\r'
	__putch '\n'
.endm

_start:
//	_putch 'L'
	/* disable all interrupts */
	movew	#0x2700, %sr

/* u-boot will give us the address of the DT in d7 */
	lea	_dtblob, %a0
	movel %d7, (%a0)
/*
 * Quickly replace the value at 0x0 with an infinite loop
 * to avoid null pointers causing havoc
 */
	movel	#0, %a0
	lea		nullcatch, %a1
	movel	(%a1), (%a0)
/*****************************************************************************
 * Setup basic memory information for kernel
 *****************************************************************************/
	movel	#CONFIG_VECTORBASE,_ramvec	/* set vector base location */

/*****************************************************************************
 * Clear bss region
 *****************************************************************************/
	lea	__bss_start, %a0		/* get start of bss */
	lea	__bss_stop, %a1			/* get end of bss */
_clear_bss:
	movel	#0, (%a0)+			/* clear each word */
	cmpl	%a0, %a1			/* check if at end */
	bne	_clear_bss

/*****************************************************************************
 * Load the current task pointer and stack.
 *****************************************************************************/
	lea	init_thread_union,%a0
	lea	THREAD_SIZE(%a0),%sp
//	_putch 'n'
	jsr	start_kernel			/* start Linux kernel */
_exit:
	jmp	_exit				/* should never get here */
