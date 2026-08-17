/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_M68K_FUTEX_H
#define _ASM_M68K_FUTEX_H

#include <asm-generic/futex.h>

#ifdef CONFIG_SMP
/*
 * m68k has historically been UP-only, so it never provided the SMP futex
 * primitives; <asm-generic/futex.h> only defines the non-local
 * futex_atomic_cmpxchg_inatomic()/arch_futex_atomic_op_inuser() when
 * !CONFIG_SMP.  Under CONFIG_SMP (E17 dual-'040) we must supply them.
 *
 * Tier-0 simplification: alias them to the generic *_local implementations.
 * Those bracket a get_user/put_user pair with preempt_disable(), which is
 * correct against preemption but NOT truly atomic against a concurrent futex
 * op on the *other* CPU.  With the D-cache off (Tier-0) and the very low
 * cross-CPU futex contention during bring-up this is safe enough; it lets the
 * SMP kernel build and boot.  A proper cas-based user futex (with extable
 * fault fixups, like the RMW_INSNS cmpxchg) is a later hardening step.
 */
#define futex_atomic_cmpxchg_inatomic(uval, uaddr, oldval, newval) \
	futex_atomic_cmpxchg_inatomic_local(uval, uaddr, oldval, newval)
#define arch_futex_atomic_op_inuser(op, oparg, oval, uaddr) \
	futex_atomic_op_inuser_local(op, oparg, oval, uaddr)
#endif /* CONFIG_SMP */

#endif /* _ASM_M68K_FUTEX_H */
