// SPDX-License-Identifier: GPL-2.0-or-later
/*
 */

#include <asm/vdso.h>
#include <linux/types.h>

/* I could not work out how to not generate a GOT entry so here we are */
static inline struct vdso_arch_data *get_arch_data(void)
{
	void *p;

        __asm__ volatile (
                "lea vdso_u_arch_data(%%pc), %0\n"
                : "=a"(p)
                :
                :
        );

	return p;
}

extern unsigned long __vdso_get_thread_area(void);
unsigned long __vdso_get_thread_area(void)
{
	struct vdso_arch_data *p = get_arch_data();

	return p->tp_value;
}
