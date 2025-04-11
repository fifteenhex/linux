/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * m68k specific definitions for NOLIBC
 * Copyright (C) 2025 Daniel Palmer<daniel@thingy.jp>
 */

#ifndef _NOLIBC_ARCH_M68K_H
#define _NOLIBC_ARCH_M68K_H

#include "compiler.h"
#include "crt.h"

/*
 */

#define _NOLIBC_SYSCALL_CLOBBERLIST "memory", "d0", "d1", "a0"

#define my_syscall0(num)                                                      \
({                                                                            \
	register long _num __asm__ ("v0") = (num);                            \
	register long _arg4 __asm__ ("a3");                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r"(_num), "=r"(_arg4)                                     \
		: "r"(_num)                                                   \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall1(num, arg1)                                                \
({                                                                            \
	register long _num __asm__ ("v0") = (num);                            \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg4 __asm__ ("a3");                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r"(_num), "=r"(_arg4)                                     \
		: "0"(_num),                                                  \
		  "r"(_arg1)                                                  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num __asm__ ("v0") = (num);                            \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("a1") = (long)(arg2);                    \
	register long _arg4 __asm__ ("a3");                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r"(_num), "=r"(_arg4)                                     \
		: "0"(_num),                                                  \
		  "r"(_arg1), "r"(_arg2)                                      \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall3(num, arg1, arg2, arg3)                                    \
({                                                                            \
	register long _num __asm__ ("v0")  = (num);                           \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("a1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("a2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("a3");                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r"(_num), "=r"(_arg4)                                     \
		: "0"(_num),                                                  \
		  "r"(_arg1), "r"(_arg2), "r"(_arg3)                          \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall4(num, arg1, arg2, arg3, arg4)                              \
({                                                                            \
	register long _num __asm__ ("v0") = (num);                            \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("a1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("a2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("a3") = (long)(arg4);                    \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r" (_num), "=r"(_arg4)                                    \
		: "0"(_num),                                                  \
		  "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4)              \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall5(num, arg1, arg2, arg3, arg4, arg5)                        \
({                                                                            \
	register long _num __asm__ ("v0") = (num);                            \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("a1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("a2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("a3") = (long)(arg4);                    \
	register long _arg5 = (long)(arg5);                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
//		"sw %7, 16($sp)\n"                                            \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r" (_num), "=r"(_arg4)                                    \
		: "0"(_num),                                                  \
		  "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5)  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

#define my_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)                  \
({                                                                            \
	register long _num __asm__ ("v0")  = (num);                           \
	register long _arg1 __asm__ ("a0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("a1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("a2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("a3") = (long)(arg4);                    \
	register long _arg5 = (long)(arg5);                                   \
	register long _arg6 = (long)(arg6);                                   \
									      \
	__asm__ volatile (                                                    \
//		"addiu $sp, $sp, -32\n"                                       \
//		"sw %7, 16($sp)\n"                                            \
//		"sw %8, 20($sp)\n"                                            \
		"trap #0\n"                                                   \
//		"addiu $sp, $sp, 32\n"                                        \
		: "=r" (_num), "=r"(_arg4)                                    \
		: "0"(_num),                                                  \
		  "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_arg6)                                                  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_arg4 ? -_num : _num;                                                 \
})

/* startup code, note that it's called __start on M68K */
void __start(void);
void __attribute__((weak, noreturn)) __nolibc_entrypoint __no_stack_protector __start(void)
{
	__asm__ volatile (
		".set push\n"
		".set noreorder\n"
		"bal 1f\n"               /* prime $ra for .cpload                            */
		"nop\n"
		"1:\n"
		".cpload $ra\n"
		"move  $a0, $sp\n"       /* save stack pointer to $a0, as arg1 of _start_c */
		"addiu $sp, $sp, -4\n"   /* space for .cprestore to store $gp              */
		".cprestore 0\n"
		"li    $t0, -8\n"
		"and   $sp, $sp, $t0\n"  /* $sp must be 8-byte aligned                     */
		"addiu $sp, $sp, -16\n"  /* the callee expects to save a0..a3 there        */
		"lui $t9, %hi(_start_c)\n" /* ABI requires current function address in $t9 */
		"ori $t9, %lo(_start_c)\n"
		"jalr $t9\n"             /* transfer to c runtime                          */
		" nop\n"                 /* delayed slot                                   */
		".set pop\n"
	);
	__nolibc_entrypoint_epilogue();
}

#endif /* _NOLIBC_ARCH_M68K_H */
