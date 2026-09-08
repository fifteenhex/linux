// SPDX-License-Identifier: GPL-2.0
/*
 * SMP futex support for the dual-'040 ELTEC EUROCOM-17.
 *
 * The 68040 has a real user-space compare-and-swap (cas/casl), but cas can only
 * address memory through the SUPERVISOR root pointer (SRP): unlike ordinary
 * loads/stores it has no alternate-address-space ("moves") form.  On m68k Linux
 * the kernel runs with SRP pointing at the kernel page tables and only URP is
 * switched per process (switch_mm_0460(); smp_secondary.S on the AP), so a
 * supervisor cas on a user virtual address never reaches the user page.  A
 * kernel-side lock would not help either: m68k userspace does its futex
 * fast-path atomics with its own cas, so the kernel side must be atomic against
 * a concurrent USER cas on the very same word.
 *
 * So resolve the user address to its physical page with a page-table walk of
 * current->mm and cas on the page's kernel linear-map alias.  The '040 data
 * cache is physically tagged and cas takes the bus lock, so a cas on the kernel
 * alias is atomic against a user-mode cas on the user alias of the same physical
 * location.
 *
 * These hooks run in atomic context (the futex core holds hb->lock and disables
 * pagefaults), so we cannot sleep.  m68k has no fast-GUP, and -- unlike arches
 * that select MMU_GATHER_RCU_TABLE_FREE -- it frees page-table pages
 * immediately (__pte_free_tlb() -> free_pointer_table()), so a purely lockless
 * walk could race free_pgtables() and touch a freed table.  Guard against that
 * with mmap_read_trylock(): free_pgtables()/mremap run under mmap_write_lock, so
 * the read lock keeps the tables alive for the walk, and holding the pte lock
 * across the cas keeps the pte/page stable against reclaim.  A trylock failure
 * or a not-present/read-only page returns -EFAULT, and the futex core faults the
 * page in (breaking COW) and retries -- the standard contract.
 *
 * m68k has no THP/hugetlb in the E17 config, so the pmd always points at a pte
 * table (no huge-pmd case to handle).
 */
#include <linux/futex.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pgtable.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/errno.h>
#include <asm/futex.h>

/* Resolved user word: the pte lock is held until futex_unlock_word(). */
struct futex_word {
	struct mm_struct *mm;
	pte_t *pte;
	spinlock_t *ptl;
	struct page *page;
	u32 *kaddr;		/* kernel linear alias of the user word */
};

/*
 * One 32-bit cas attempt on the kernel alias.  Returns the value that was in
 * memory before the attempt: equal to @expect if the swap happened, otherwise
 * the current (unchanged) value.  The pte lock is held and the page is present
 * and writable, so the cas itself cannot fault.
 */
static inline u32 futex_casl(u32 *kaddr, u32 expect, u32 newval)
{
	__asm__ __volatile__ ("casl %0,%2,%1"
			      : "+d" (expect), "+m" (*kaddr)
			      : "d" (newval)
			      : "memory");
	return expect;
}

/*
 * Resolve @uaddr to its kernel alias and leave the pte lock (and mmap read lock)
 * held.  On success the caller operates on w->kaddr and then calls
 * futex_unlock_word().  On failure everything is released and -EFAULT returned.
 */
static int futex_lock_word(u32 __user *uaddr, struct futex_word *w)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr = (unsigned long)uaddr;
	struct vm_area_struct *vma;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t entry;
	struct page *page;
	void *kva;

	if (!mm || !access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	/* Blocks free_pgtables()/mremap (mmap_write_lock); trylock: never sleep. */
	if (!mmap_read_trylock(mm))
		return -EFAULT;

	vma = vma_lookup(mm, addr);
	if (!vma || !(vma->vm_flags & VM_WRITE))
		goto efault_mm;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		goto efault_mm;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		goto efault_mm;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		goto efault_mm;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
		goto efault_mm;

	w->pte = pte_offset_map_lock(mm, pmd, addr, &w->ptl);
	if (!w->pte)
		goto efault_mm;
	entry = *w->pte;
	if (!pte_present(entry) || !pte_write(entry))
		goto efault_ptl;
	page = vm_normal_page(vma, addr, entry);	/* excludes special/PFNMAP */
	if (!page)
		goto efault_ptl;
	kva = page_address(page);
	if (!kva)			/* highmem: E17 is lowmem-only, cannot happen */
		goto efault_ptl;

	w->mm = mm;
	w->page = page;
	w->kaddr = kva + (addr & ~PAGE_MASK);
	return 0;

efault_ptl:
	pte_unmap_unlock(w->pte, w->ptl);
efault_mm:
	mmap_read_unlock(mm);
	return -EFAULT;
}

/* Release the word.  @wrote marks a file-backed page dirty (we bypass the user
 * pte, so the hardware dirty bit was not set).  set_page_dirty() under the pte
 * lock is safe -- zap_pte_range() does the same. */
static void futex_unlock_word(struct futex_word *w, bool wrote)
{
	if (wrote)
		set_page_dirty(w->page);
	pte_unmap_unlock(w->pte, w->ptl);
	mmap_read_unlock(w->mm);
}

int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval, u32 __user *uaddr)
{
	struct futex_word w;
	u32 old, new;
	int ret;

	ret = futex_lock_word(uaddr, &w);
	if (ret)
		return ret;

	/* Lock-free RMW: recompute and retry until our cas wins the word. */
	do {
		old = READ_ONCE(*w.kaddr);
		switch (op) {
		case FUTEX_OP_SET:  new = oparg;        break;
		case FUTEX_OP_ADD:  new = old + oparg;  break;
		case FUTEX_OP_OR:   new = old | oparg;  break;
		case FUTEX_OP_ANDN: new = old & ~oparg; break;
		case FUTEX_OP_XOR:  new = old ^ oparg;  break;
		default:
			futex_unlock_word(&w, false);
			return -ENOSYS;
		}
	} while (futex_casl(w.kaddr, old, new) != old);

	futex_unlock_word(&w, true);
	*oval = old;
	return 0;
}

int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
				  u32 oldval, u32 newval)
{
	struct futex_word w;
	u32 cur;
	int ret;

	ret = futex_lock_word(uaddr, &w);
	if (ret)
		return ret;

	cur = futex_casl(w.kaddr, oldval, newval);
	futex_unlock_word(&w, cur == oldval);
	*uval = cur;
	return 0;
}
