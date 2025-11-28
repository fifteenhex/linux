#ifndef __ASM_VDSO_H
#define __ASM_VDSO_H

#ifndef __ASSEMBLY__

#include <linux/mm_types.h>
#include <vdso/datapage.h>

struct m68k_vdso_info {
	void *vdso;
	unsigned long size;
	struct vm_special_mapping code_mapping;
};

extern struct m68k_vdso_info vdso_info;

#endif

#endif /* __ASM_VDSO_H */
