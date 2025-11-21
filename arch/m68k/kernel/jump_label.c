// SPDX-License-Identifier: GPL-2.0
#include <asm/cacheflush.h>
#include <linux/memory.h>

static void __toggle_wp(unsigned long virtaddr, ssize_t size, bool set)
{
	pgd_t *pgd_dir;
	p4d_t *p4d_dir;
	pud_t *pud_dir;
	pmd_t *pmd_dir;
	pte_t *pte_dir;

	while (size > 0) {
		pgd_dir = pgd_offset_k(virtaddr);
		p4d_dir = p4d_offset(pgd_dir, virtaddr);
		pud_dir = pud_offset(p4d_dir, virtaddr);
		if (pud_bad(*pud_dir)) {
			pud_clear(pud_dir);
			return;
		}
		pmd_dir = pmd_offset(pud_dir, virtaddr);

#if CONFIG_PGTABLE_LEVELS == 3
		if (CPU_IS_020_OR_030) {
			unsigned long pmd = pmd_val(*pmd_dir);

			if ((pmd & _DESCTYPE_MASK) == _PAGE_PRESENT) {
				if (set)
					*pmd_dir = __pmd(pmd | _PAGE_RONLY);
				else
					*pmd_dir = __pmd(pmd & ~_PAGE_RONLY);
				virtaddr += PMD_SIZE;
				size -= PMD_SIZE;
				continue;
			}
		}
#endif

		if (pmd_bad(*pmd_dir)) {
			pmd_clear(pmd_dir);
			return;
		}

		pte_dir = pte_offset_kernel(pmd_dir, virtaddr);

		if (set)
			set_pte(pte_dir, pte_wrprotect(*pte_dir));
		else
			set_pte(pte_dir,  pte_mkwrite_novma(*pte_dir));
		virtaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}
}

void arch_jump_label_transform(struct jump_entry *entry, enum jump_label_type type)
{
	unsigned long addr = entry->code;
	u32 *insn;
	u32 val;
	int ret;

	insn = (u32 *) addr;

	if (type == JUMP_LABEL_JMP) {
		s16 disp = (entry->target - (addr + 2));
		val = (0x60000000 | disp);
	}
	else
		val = 0x4e714e71;

	mutex_lock(&text_mutex);

	/* Page was WP, unset wp.. */
	ret = copy_to_kernel_nofault(insn, &val, sizeof(val));
	if (ret) {
		__toggle_wp(addr, sizeof(val), false);
		copy_to_kernel_nofault(insn, &val, sizeof(val));
		__toggle_wp(addr, sizeof(val), true);
	}

	mutex_unlock(&text_mutex);

	flush_icache_range(addr, addr + JUMP_LABEL_NOP_SIZE);
}
