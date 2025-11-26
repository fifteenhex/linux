/* SPDX-License-Identifier: GPL-2.0 */
/*
 * runtime const implementation for m68k
 * Copyright (C) 2025 Daniel Palmer<daniel@thingy.jp>
 *
 * Based on arm64 version.
 */
#ifndef _ASM_RUNTIME_CONST_H
#define _ASM_RUNTIME_CONST_H

#include <asm/cacheflush.h>
#include <linux/bitfield.h>

#define runtime_const_ptr(sym) ({				\
	typeof(sym) __ret;					\
	asm_inline("1:\t"					\
		"mov.l #0xcafef00d,%0\n\t"			\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: "=a" (__ret));				\
	__ret; })

static inline void __runtime_fixup_ptr(void *where, unsigned long val)
{
	u32 *value = where + 2;
	const unsigned long start = ((unsigned long) where) + 2;
	const unsigned long end = start + sizeof(*value);

	*value = val;

	flush_icache_range(start, end);
}

#ifdef CONFIG_M68000
/* How many shift by 8 instructions are in a shift constant */
#define SHIFT_32_INSNS (32 / 8)
/* Size of each shift instruction */
#define SHIFT_INSN_SZ 2

/* Generate enough shift by 8s to shift up to 32 places */
#define runtime_const_shift_right_32(val, sym) ({		\
	asm_inline("1:\t"					\
		"lsr.l #8, %0\n\t"				\
		"lsr.l #8, %0\n\t"				\
		"lsr.l #8, %0\n\t"				\
		"lsr.l #8, %0\n\t"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: "+d" (val));					\
	val; })

#define LSR_IMM_REG_COUNT GENMASK(11, 9)
#define NOP 0x4e71

static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	const unsigned long shifts_start = (unsigned long) where;
	u16 *insn = where;

	for (int i = 0; i < SHIFT_32_INSNS; i++, insn++) {
		if (val >= 8) {
			/* Existing instruction shifts by 8, just subtract 8 from val */
			val -= 8;
		} else if (val > 0) {
			/* Replace the shift count with val */
			FIELD_MODIFY(LSR_IMM_REG_COUNT, insn, val);
			val = 0;
		} else {
			/* Replace unneeded shift with nop */
			*insn = NOP;
		}
	}

	flush_icache_range(shifts_start, shifts_start + (SHIFT_32_INSNS * SHIFT_INSN_SZ));
}
#else
#define runtime_const_shift_right_32(val, sym) ({		\
	u32 __tmp;						\
	asm_inline("1:\t"					\
		"moveq #12, %0\n\t"				\
		"lsr.l %0, %1\n\t"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: "=&d" (__tmp), "+d" (val));			\
	val; })

#define MOVEQ_DATA GENMASK(7, 0)
static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	u16 *insn = where;
	const unsigned long start = (unsigned long) where;
	const unsigned long end = start + sizeof(*insn);

	FIELD_MODIFY(MOVEQ_DATA, insn, val);

	flush_icache_range(start, end);
}
#endif

#define runtime_const_init(type, sym) do {		\
	extern s32 __start_runtime_##type##_##sym[];	\
	extern s32 __stop_runtime_##type##_##sym[];	\
	runtime_const_fixup(__runtime_fixup_##type,	\
		(unsigned long)(sym),			\
		__start_runtime_##type##_##sym,		\
		__stop_runtime_##type##_##sym);		\
} while (0)

static inline void runtime_const_fixup(void (*fn)(void *, unsigned long),
	unsigned long val, s32 *start, s32 *end)
{
	while (start < end) {
		fn(*start + (void *)start, val);
		start++;
	}
}

#endif
