/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MMU_H
#define __MMU_H

#ifdef CONFIG_MMU
typedef struct {
	unsigned long x;
	void *vdso;
} mm_context_t;
#else
#include <asm-generic/mmu.h>
#endif

#endif
