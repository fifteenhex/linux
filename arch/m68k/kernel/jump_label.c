// SPDX-License-Identifier: GPL-2.0
#include <asm/cacheflush.h>
#include <linux/memory.h>

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
	} else
		val = 0x4e714e71;

	mutex_lock(&text_mutex);

	/*
	 * This will need to be changed to remove the WP bit from the affected page(s)
	 * before the update and to add back the bit after the update once we get kernel
	 * memory protection.
	 */
	copy_to_kernel_nofault(insn, &val, sizeof(val));

	mutex_unlock(&text_mutex);

	flush_icache_range(addr, addr + JUMP_LABEL_NOP_SIZE);
}
