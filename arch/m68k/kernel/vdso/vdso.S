/* SPDX-License-Identifier: GPL-2.0-only */
/*
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/page.h>

	.globl vdso_start, vdso_end
	.section .rodata
	.balign PAGE_SIZE
vdso_start:
	.incbin "arch/m68k/kernel/vdso/vdso.so"
	.balign PAGE_SIZE
vdso_end:

	.previous
