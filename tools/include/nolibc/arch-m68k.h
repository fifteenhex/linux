/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * m68k specific definitions for NOLIBC
 * Copyright (C) 2025 Daniel Palmer<daniel@thingy.jp>
 *
 * Roughly based on one or more of the other arch files.
 *
 */

#ifndef _NOLIBC_ARCH_M68K_H
#define _NOLIBC_ARCH_M68K_H

#include "compiler.h"
#include "crt.h"
#include "elf.h"

#define _NOLIBC_SYSCALL_CLOBBERLIST "memory"

#define my_syscall0(num)                                                      \
({                                                                            \
	register long _num __asm__ ("d0") = (num);                            \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r"(_num)                                                  \
		: "r"(_num)                                                   \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall1(num, arg1)                                                \
({                                                                            \
	register long _num __asm__ ("d0") = (num);                            \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r"(_num)                                                  \
		: "r"(_arg1)                                                  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num __asm__ ("d0") = (num);                            \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
	register long _arg2 __asm__ ("d2") = (long)(arg2);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r"(_num)                                                  \
		: "r"(_arg1), "r"(_arg2)                                      \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall3(num, arg1, arg2, arg3)                                    \
({                                                                            \
	register long _num __asm__ ("d0")  = (num);                           \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
	register long _arg2 __asm__ ("d2") = (long)(arg2);                    \
	register long _arg3 __asm__ ("d3") = (long)(arg3);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r"(_num)                                                  \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3)                          \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall4(num, arg1, arg2, arg3, arg4)                              \
({                                                                            \
	register long _num __asm__ ("d0") = (num);                            \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
	register long _arg2 __asm__ ("d2") = (long)(arg2);                    \
	register long _arg3 __asm__ ("d3") = (long)(arg3);                    \
	register long _arg4 __asm__ ("d4") = (long)(arg4);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r" (_num)                                                 \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4)              \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall5(num, arg1, arg2, arg3, arg4, arg5)                        \
({                                                                            \
	register long _num __asm__ ("d0") = (num);                            \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
	register long _arg2 __asm__ ("d2") = (long)(arg2);                    \
	register long _arg3 __asm__ ("d3") = (long)(arg3);                    \
	register long _arg4 __asm__ ("d4") = (long)(arg4);                    \
	register long _arg5 __asm__ ("d5") = (long)(arg5);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r" (_num)                                                 \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5)  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#define my_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)                  \
({                                                                            \
	register long _num __asm__ ("d0")  = (num);                           \
	register long _arg1 __asm__ ("d1") = (long)(arg1);                    \
	register long _arg2 __asm__ ("d2") = (long)(arg2);                    \
	register long _arg3 __asm__ ("d3") = (long)(arg3);                    \
	register long _arg4 __asm__ ("d4") = (long)(arg4);                    \
	register long _arg5 __asm__ ("d5") = (long)(arg5);                    \
	register long _arg6 __asm__ ("a0") = (long)(arg6);                    \
									      \
	__asm__ volatile (                                                    \
		"trap #0\n"                                                   \
		: "+r" (_num)                                                 \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_arg6)                                                  \
		: _NOLIBC_SYSCALL_CLOBBERLIST                                 \
	);                                                                    \
	_num;                                                                 \
})

#ifdef NOLIBC_STATIC_PIE
#define R_68K_RELATIVE	22
void _relocate(void);
void __no_stack_protector _relocate(void) {
	extern char _DYNAMIC[];
	unsigned int rela_count = 0;
	unsigned int rela_off = 0;
	unsigned long base_addr;
	unsigned long dyn_addr;
	Elf32_Rela *rela;
	Elf32_Addr *addr;
	Elf32_Dyn *dyn;
	void *base;
	int i;

	/* For m68k with the FDPIC loader d5 contains the offset to the DYNAMIC segment */
	__asm__ volatile (
		"move.l %%d5, %0\n"
		: "=r" (dyn_addr)
	);
	dyn = (Elf32_Dyn *) dyn_addr;

	base_addr = dyn_addr - (unsigned long) _DYNAMIC;
	base = (void *) base_addr;

	/* Go through the DYNAMIC segment and get the offset to rela and the number of relocations */
	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:
			rela_off = dyn->d_un.d_ptr;
			break;
		case DT_RELACOUNT:
			rela_count = dyn->d_un.d_val;
			break;
		}
	}

	if (!rela_off || !rela_count)
		return;

	rela = base + rela_off;

	/* Do the relocations, only R_68K_RELATIVE for now */
	for (i = 0; i < rela_count; i++) {
		Elf32_Rela *entry = &rela[i];

		switch (ELF32_R_TYPE(entry->r_info)) {
		case R_68K_RELATIVE:
		{
			addr = (Elf32_Addr *)(base + entry->r_offset);
			*addr = (Elf32_Addr) (base + entry->r_addend);
		}
			break;
		default:
			return;
		}
	}
}
#endif

#ifndef NOLIBC_NO_RUNTIME
void _start(void);
void __attribute__((weak, noreturn)) __nolibc_entrypoint __no_stack_protector _start(void)
{
	__asm__ volatile (
		"movel %sp, %sp@-\n"
#ifdef NOLIBC_STATIC_PIE
		"lea _relocate(%pc), %a0\n"
		"jsr (%a0)\n"
#endif
		"lea _start_c(%pc), %a0\n"
		"jsr (%a0)\n"
	);
	__nolibc_entrypoint_epilogue();
}
#endif /* NOLIBC_NO_RUNTIME */

#endif /* _NOLIBC_ARCH_M68K_H */
