/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _M68K_BARRIER_H
#define _M68K_BARRIER_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>	/* barrier() */

/*
 * cpu_relax() is also defined (identically) in <asm/processor.h>, but the
 * generic SMP primitives in <asm-generic/barrier.h> (smp_cond_load_*) and the
 * asm-generic ticket spinlock use it and can be pulled in before processor.h.
 * Define it here too so those always compile.  Identical macro redefinition is
 * permitted by C, so including both headers is harmless.
 */
#define cpu_relax()	barrier()

/*
 * m68k has no explicit memory-barrier instruction, so asm-generic's mb()/
 * smp_mb() reduce to a compiler barrier().  That is sufficient on the E17's
 * dual '040: memory is mapped write-through and the '040 completes bus accesses
 * in program order, and the SMP RMW primitives use the bus-locking cas/casl
 * instructions -- i.e. the coherence model is effectively TSO, so a compiler
 * barrier is all the ordering the generated code needs.
 */

#endif /* __ASSEMBLY__ */

#include <asm-generic/barrier.h>

#endif /* _M68K_BARRIER_H */
