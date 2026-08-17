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

#endif /* __ASSEMBLY__ */

#include <asm-generic/barrier.h>

#endif /* _M68K_BARRIER_H */
