/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_M68K_EXCEPTION_H
#define _ASM_M68K_EXCEPTION_H

#include <asm/ptrace.h>
#include <linux/linkage.h>

asmlinkage void noinstr m68000_irq_handler(struct pt_regs *regs);

#endif /* _ASM_M68K_EXCEPTION_H */
