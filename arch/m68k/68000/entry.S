/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  entry.S -- non-mmu 68000 interrupt and exception entry points
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 * Linux/m68k support by Hamish Macdonald
 */

#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/errno.h>
#include <asm/setup.h>
#include <asm/traps.h>
#include <asm/asm-offsets.h>
#include <asm/entry.h>

.text

.globl system_call
.globl system_call_68000
.globl resume
.globl ret_from_exception
.globl sys_call_table
.globl bad_interrupt

/*
 * Reorder the frame and insert the vector so 68000 looks
 * like 68010+, must be undone before returning
 */
 .macro fixframe num
	/* disable intrs */
	move	#0x2700,%sr
	/* restack pc and sr */
	movew	%sp@, %sp@-
	movew	%sp@(4), %sp@(2)
	movew	%sp@(6), %sp@(4)
	/* insert vector number*/
	movew	#(\num * 4), %sp@(6)
.endm

badsys:
	movel	#-ENOSYS,%sp@(PT_OFF_D0)
	jra	ret_from_exception

do_trace:
	movel	#-ENOSYS,%sp@(PT_OFF_D0) /* needed for strace*/
	subql	#4,%sp
	SAVE_SWITCH_STACK
	jbsr	syscall_trace_enter
	RESTORE_SWITCH_STACK
	addql	#4,%sp
	addql	#1,%d0
	jeq	ret_from_exception
	movel	%sp@(PT_OFF_ORIG_D0),%d1
	movel	#-ENOSYS,%d0
	cmpl	#NR_syscalls,%d1
	jcc	1f
	lsl	#2,%d1
	lea	sys_call_table, %a0
	jbsr	%a0@(%d1)

1:	movel	%d0,%sp@(PT_OFF_D0)	/* save the return value */
	subql	#4,%sp			/* dummy return address */
	SAVE_SWITCH_STACK
	jbsr	syscall_trace_leave
	RESTORE_SWITCH_STACK
	addql	#4,%sp
	jra	ret_from_exception

/* get thread_info pointer */
 .macro getthreadinfo reg
	movel	%sp,%d1
	andl	#-THREAD_SIZE,%d1
	movel	%d1,\reg
 .endm

ENTRY(system_call_68000)
	fixframe 32
	bra _syscall

ENTRY(system_call)
_syscall:
	SAVE_ALL_SYS

	/* save top of frame*/
	pea	%sp@
	jbsr	set_esp0
	addql	#4,%sp

	movel	%sp@(PT_OFF_ORIG_D0),%d0

	/* Doing a trace ? */
	getthreadinfo %a2
	btst	#(TIF_SYSCALL_TRACE%8),%a2@(TINFO_FLAGS+(31-TIF_SYSCALL_TRACE)/8)
	jne	do_trace

	cmpl	#NR_syscalls,%d0
	jcc	badsys
syscall:
	lsl	#2,%d0
	lea	sys_call_table,%a0
	movel	%a0@(%d0), %a0
	jbsr	%a0@
ret_from_syscall:
	movel	%d0,%sp@(PT_OFF_D0)	/* save the return value*/

ret_from_exception:
	btst	#5,%sp@(PT_OFF_SR)	/* check if returning to kernel*/
	jeq	Luser_return		/* if so, skip resched, signals*/

Lkernel_return:
	RESTORE_ALL

Luser_return:
	/* only allow interrupts when we are really the last one on the*/
	/* kernel stack, otherwise stack overflow can occur during*/
	/* heavy interrupt load*/
	andw	#ALLOWINT,%sr

	getthreadinfo %a2
1:
	/* check if any of the flags are set */
	movel	%a2@(TINFO_FLAGS),%d1	/* thread_info->flags */
	jne	Lwork_to_do
	RESTORE_ALL

Lwork_to_do:
	/* check if reschedule needs to be called */
	btst	#TIF_NEED_RESCHED,%d1
	jne	reschedule

Lsignal_return:
	subql	#4,%sp			/* dummy return address*/
	SAVE_SWITCH_STACK
	pea	%sp@(SWITCH_STACK_SIZE)
	bsrw	do_notify_resume
	addql	#4,%sp
	RESTORE_SWITCH_STACK
	addql	#4,%sp
	jra	1b

.macro inthandlerbody func
	SAVE_ALL_INT

	/* Push frame address onto stack */
	pea	%sp@
	/* process the IRQ*/
	jbsr	\func
	/* pop parameters off stack*/
	addql	#4,%sp
	bra	ret_from_exception
.endm

/* Create an interrupt vector sled */
 .macro inthandler num func
	.globl inthandler\num
	inthandler\num:
	fixframe \num
	inthandlerbody \func
 .endm

/* processor exceptions */
.globl inthandler_cpuexception
inthandler_cpuexception:
	inthandlerbody process_int_oops
/* shit happened vectors */
inthandler 2 process_int_oops
inthandler 3 process_int_oops
inthandler 4 process_int_oops
inthandler 5 process_int_oops
inthandler 6 process_int_oops
inthandler 7 process_int_oops
inthandler 8 process_int_oops

/* auto vectored interrupt handlers */
/* 68010+ */
.globl inthandler_autovec
inthandler_autovec:
	inthandlerbody process_int_autovec
#ifdef CONFIG_M68000
/* 68000 */
inthandler 25 process_int_autovec
inthandler 26 process_int_autovec
inthandler 27 process_int_autovec
inthandler 28 process_int_autovec
inthandler 29 process_int_autovec
inthandler 30 process_int_autovec
inthandler 31 process_int_autovec
#endif

.globl inthandler_badtrap
inthandler_badtrap:
	inthandlerbody process_int_badtrap
#ifdef CONFIG_M68000
/* 68000 */
inthandler 33 process_int_badtrap
#endif

/* Dragonball interrupts */
#ifdef CONFIG_M68000
inthandler 65 process_int
inthandler 66 process_int
inthandler 67 process_int
inthandler 68 process_int
inthandler 69 process_int
inthandler 70 process_int
inthandler 71 process_int
#endif

/*
 * Handler for uninitialized and spurious interrupts.
 */
ENTRY(bad_interrupt)
	addql	#1,irq_err_count
	rte

/*
 * Beware - when entering resume, prev (the current task) is
 * in a0, next (the new task) is in a1, so don't change these
 * registers until their contents are no longer needed.
 */
ENTRY(resume)
	/* save prev thread in d1 */
	movel	%a0,%d1
	/* save sr */
	movew	%sr,%a0@(TASK_THREAD+THREAD_SR)
	SAVE_SWITCH_STACK
	/* save kernel stack */
	movel	%sp,%a0@(TASK_THREAD+THREAD_KSP)
	/* save usp */
	movel	%usp,%a3
	movel	%a3,%a0@(TASK_THREAD+THREAD_USP)

	/* restore user stack */
	movel	%a1@(TASK_THREAD+THREAD_USP),%a3
	movel	%a3,%usp
	/* restore new thread stack */
	movel	%a1@(TASK_THREAD+THREAD_KSP),%sp
	RESTORE_SWITCH_STACK
	/* restore thread status reg */
	movew	%a1@(TASK_THREAD+THREAD_SR),%sr
	rts

