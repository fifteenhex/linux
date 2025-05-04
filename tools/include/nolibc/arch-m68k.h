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

void _start(void);
void __attribute__((weak, noreturn)) __nolibc_entrypoint __no_stack_protector _start(void)
{
	__asm__ volatile (
		"movel %sp, %sp@-\n"
		"jsr _start_c\n"
	);
	__nolibc_entrypoint_epilogue();
}

#define NOLIBC_ARCH_HAS_MEMCPY

#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)
#define WORD_ALIGN
#endif

#if !(defined(__mc68040__) || defined(__mc68060__))
#define HEAVY_UNROLL	1
#else
#define HEAVY_UNROLL	0
#define HAVE_MOVE16
#endif

void *memcpy(void *dst, const void *src, size_t len);
__attribute__((weak,unused,section(".text.nolibc_memcpy")))
void *memcpy(void *dst, const void *src, size_t len)
{
	const unsigned int blocksz = HEAVY_UNROLL ? 128 : 32;
        size_t pos = 0, stop, residual;

#ifdef WORD_ALIGN
	/*
	 * 000 cannot handle non-word aligned accesses for >byte thus
	 * src and dst need word aligned before any fancy stuff so we need
	 * to do a byte copy first in that case. If its not possible to align
	 * both we fallback to byte by byte.
	 */
	{
		unsigned int required_alignment = sizeof(unsigned short);
		size_t dst_misalignment = (uintptr_t)dst & (required_alignment - 1);
		const size_t src_misalignment = (uintptr_t)src & (required_alignment - 1);
		char *cdst = (char *)(dst + pos);
		const char *csrc = (const char *)(src + pos);

		if (dst_misalignment != src_misalignment)
			goto bytebybyte;

		/* If we are already aligned just start chomping */
		if (!dst_misalignment)
			goto blockbyblock;

		/* do a byte copy to get word aligned */
		*cdst = *csrc;
		pos++;
	}
#endif

#ifdef HAVE_MOVE16
	/*
	 * If you have move16 it's super fast but the pointers need to be
	 * aligned to 16 so it's not so simple to actually pull off...
	 */
	{
		const size_t chunksz = 128;
		const unsigned int chunkalign = 16;
		const size_t dst_misalignment = (uintptr_t)dst & (chunkalign - 1);
		const size_t src_misalignment = (uintptr_t)src & (chunkalign - 1);
		void *tdst;
		const void *tsrc;

		/* Is the copy big enough? */
		residual = len - pos;
		if (residual < chunksz)
			goto blockbyblock;

		/* Is it even possible to align the pointers together? */
		if (dst_misalignment != src_misalignment)
			goto blockbyblock;

		/* Already aligned, go go go */
		if (!dst_misalignment)
			goto chunkbychunk;

		/* Fix up the alignment doing a few unsigned longs first */
		stop = pos + chunkalign;
		while (pos < stop) {
			unsigned long *ldst = (unsigned long*)(dst + pos);
			unsigned long *lsrc = (unsigned long*)(src + pos);
			*ldst = *lsrc;
			pos += sizeof(unsigned long);
		}

		/* Fix up overshoot from the above */
		pos -= dst_misalignment;
chunkbychunk:
		residual = len - pos;
		if (residual < chunksz)
			goto blockbyblock;

		/* send it */
		stop = pos + (residual & ~(chunksz - 1));
		while (pos < stop) {
			tsrc = src + pos;
			tdst = dst + pos;
			asm volatile (".rept 8\n"
				      "move16 (%[from])+, (%[to])+\n"
				      ".endr\n"
				      : [from] "+a" (tsrc), [to] "+a" (tdst));
			pos += chunksz;
		}
	}
#endif

	/*
	 * On 040 unrolling does nothing after 16 to 32 bytes,
	 * on 030 it makes a big difference until 128 bytes
	 */
blockbyblock:
	/* Is the copy big enough ?*/
	residual = len - pos;
	if (residual < blocksz)
		goto longbylong;

	stop = pos + (residual & ~(blocksz - 1));
	while (pos < stop) {
		unsigned long *ldst = (unsigned long*)(dst + pos);
		const unsigned long *lsrc = (const unsigned long*)(src + pos);

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 16 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 32 bytes */

#if HEAVY_UNROLL
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 48 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 64 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 80 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 96 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 112 bytes */

		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		*ldst++ = *lsrc++;
		/* 128 bytes */
#endif

		pos += blocksz;
	}

longbylong:
	/* is the copy big enough? */
	residual = len - pos;
	if (residual < sizeof(unsigned long))
		goto wordbyword;

	stop = pos + (residual & ~(sizeof(unsigned long) - 1));
	while (pos < stop) {
		unsigned long *ldst = (unsigned long*)(dst + pos);
		const unsigned long *lsrc = (const unsigned long*)(src + pos);
		*ldst = *lsrc;
		pos += sizeof(unsigned long);
	}

wordbyword:
	/* is the copy big enough? */
	residual = len - pos;
	if (residual < sizeof(unsigned short))
		goto bytebybyte;

	stop = pos + (residual & ~(sizeof(unsigned short) - 1));
	while (pos < stop) {
		unsigned short *sdst = (unsigned short*)(dst + pos);
		const unsigned short *ssrc = (const unsigned short*)(src + pos);
		*sdst = *ssrc;
		pos += sizeof(unsigned short);
	}

bytebybyte:
	while (pos < len) {
		unsigned char *cdst = (unsigned char*)(dst + pos);
		unsigned char *csrc = (unsigned char*)(src + pos);
		*cdst = *csrc;
		pos++;
	}

        return dst;
}

#endif /* _NOLIBC_ARCH_M68K_H */
