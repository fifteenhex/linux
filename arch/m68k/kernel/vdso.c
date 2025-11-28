/* */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/vdso_datastore.h>
#include <vdso/datapage.h>
#include <asm/elf.h>
#include <asm/vdso.h>


extern char vdso_start[], vdso_end[];

static int vdso_mremap(const struct vm_special_mapping *sm, struct vm_area_struct *new_vma)
{
	current->mm->context.vdso = (void *)(new_vma->vm_start);

	return 0;
}

struct m68k_vdso_info vdso_info = {
	.vdso = vdso_start,
	.code_mapping = {
		.name = "[vdso]",
		.mremap = vdso_mremap,
	},
};

static int __init vdso_init(void)
{
	unsigned long i, pfn;

	BUG_ON(!PAGE_ALIGNED(vdso_info.vdso));

	vdso_info.size = PAGE_ALIGN(vdso_end - vdso_start);
	vdso_info.code_mapping.pages =
		kcalloc(vdso_info.size / PAGE_SIZE, sizeof(struct page *), GFP_KERNEL);

	if (!vdso_info.code_mapping.pages)
		return -ENOMEM;

	pfn = __phys_to_pfn(__pa_symbol(vdso_info.vdso));
	for (i = 0; i < vdso_info.size / PAGE_SIZE; i++)
		vdso_info.code_mapping.pages[i] = pfn_to_page(pfn + i);

	return 0;
}
arch_initcall(vdso_init);

int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	int ret;
	unsigned long size, data_addr, vdso_addr;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	//struct m68k_vdso_info *info = current->context.vdso;

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	/*
	 * Determine total area size. This includes the VDSO data itself
	 * and the data pages.
	 */
	size = VDSO_NR_PAGES * PAGE_SIZE;

	data_addr = get_unmapped_area(NULL, STACK_TOP, size, 0, 0);
	if (IS_ERR_VALUE(data_addr)) {
		ret = data_addr;
		goto out;
	}

	vma = vdso_install_vvar_mapping(mm, data_addr);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	vdso_addr = data_addr + size;
	vma = _install_special_mapping(mm, vdso_addr, vdso_info.size,
					VM_READ | VM_EXEC |
					VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC,
				       &vdso_info.code_mapping);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	mm->context.vdso = (void *)vdso_addr;
	ret = 0;

out:
	mmap_write_unlock(mm);
	return ret;
}
