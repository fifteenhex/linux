// SPDX-License-Identifier: GPL-2.0
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <linux/memory.h>
#include <linux/uaccess.h>

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

/* Is the (kernel) text page covering addr currently mapped read-only? */
static bool text_is_ro(unsigned long addr)
{
	pgd_t *pgd_dir = pgd_offset_k(addr);
	p4d_t *p4d_dir = p4d_offset(pgd_dir, addr);
	pud_t *pud_dir = pud_offset(p4d_dir, addr);
	pmd_t *pmd_dir;
	pte_t *pte_dir;

	if (pud_bad(*pud_dir))
		return false;
	pmd_dir = pmd_offset(pud_dir, addr);

#if CONFIG_PGTABLE_LEVELS == 3
	if (CPU_IS_020_OR_030) {
		unsigned long pmd = pmd_val(*pmd_dir);

		if ((pmd & _DESCTYPE_MASK) == _PAGE_PRESENT)
			return !!(pmd & _PAGE_RONLY);
	}
#endif
	if (pmd_bad(*pmd_dir))
		return false;
	pte_dir = pte_offset_kernel(pmd_dir, addr);
	return !pte_write(*pte_dir);
}

void arch_jump_label_transform(struct jump_entry *entry, enum jump_label_type type)
{
	unsigned long addr = entry->code;
	u32 *insn;
	u32 val;
	bool ro;

	insn = (u32 *) addr;

	if (type == JUMP_LABEL_JMP) {
		s16 disp = (entry->target - (addr + 2));
		val = 0x60000000 | (disp & 0xffff);	/* bra.w disp */
	}
	else
		val = 0x4e714e71;			/* nop; nop */

	mutex_lock(&text_mutex);

	/*
	 * With STRICT_KERNEL_RWX the text is mapped read-only once
	 * mark_rodata_ro() has run.  Do NOT rely on a probing write to
	 * fault and be recovered: on the 68040 a write-protection fault is
	 * reported through a deferred writeback whose PC does not match the
	 * faulting store, so the copy_*_nofault exception fixup is not
	 * applied and the kernel oopses.  Instead only drop the protection
	 * when the page really is read-only (jump_label_init runs *before*
	 * mark_rodata_ro, while text is still writable - toggling it RO
	 * then would prematurely protect the text and corrupt later boot).
	 * Flush the ATC so the writable mapping is live, patch, then
	 * restore the original protection.
	 */
	ro = text_is_ro(addr);
	if (ro) {
		__toggle_wp(addr, sizeof(val), false);
		flush_tlb_all();
	}
	copy_to_kernel_nofault(insn, &val, sizeof(val));
	if (ro) {
		__toggle_wp(addr, sizeof(val), true);
		flush_tlb_all();
	}

	mutex_unlock(&text_mutex);

	flush_icache_range(addr, addr + JUMP_LABEL_NOP_SIZE);
}
