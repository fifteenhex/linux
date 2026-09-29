/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Suspend to RAM handshake between the kernel and the first stage loader.
 *
 * Sleep on these chips ends in a soft reset: the DRAM is left in self
 * refresh, the SoC restarts through the boot ROM and the first stage
 * loader brings the DRAM controller back up. Before the reset the kernel
 * fills this block at the top of the on chip SRAM, which the reset does
 * not touch. The loader looks at it once DRAM is usable and, if it says
 * we are asleep, jumps to the resume entry instead of booting.
 */
#ifndef _SOC_MSTAR_STR_H_
#define _SOC_MSTAR_STR_H_

#define MSTAR_STR_INFO_PHYS		0xa000ff00
#define MSTAR_STR_MAGIC			0x52545353	/* "SSTR" */
#define MSTAR_STR_STATUS_AWAKE		0
#define MSTAR_STR_STATUS_ASLEEP		1

struct mstar_str_info {
	u32 magic;
	u32 resume;	/* physical address to jump to, mmu and caches off */
	u32 status;
	u32 check;	/* ~(magic ^ resume ^ status) */
};

static inline u32 mstar_str_check(u32 magic, u32 resume, u32 status)
{
	return ~(magic ^ resume ^ status);
}

#endif
