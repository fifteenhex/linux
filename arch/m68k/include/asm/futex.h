/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_M68K_FUTEX_H
#define _ASM_M68K_FUTEX_H

#include <asm-generic/futex.h>

#ifdef CONFIG_SMP
/*
 * m68k has historically been UP-only, so <asm-generic/futex.h> only defines the
 * default futex primitives when !CONFIG_SMP (there they rely on preempt_disable
 * for mutual exclusion).  Under CONFIG_SMP (E17 dual-'040) we must supply real
 * cross-CPU-atomic versions.  They are true compare-and-swap operations, but
 * because the '040 cas cannot address user space from supervisor mode they
 * cas on the user page's kernel alias -- see arch/m68k/kernel/futex.c.
 */
int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval, u32 __user *uaddr);
int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
				  u32 oldval, u32 newval);
#endif /* CONFIG_SMP */

#endif /* _ASM_M68K_FUTEX_H */
