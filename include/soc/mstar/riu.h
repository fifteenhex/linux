/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _SOC_MSTAR_RIU_H_
#define _SOC_MSTAR_RIU_H_

#include <linux/io.h>

static inline u32 riu_readl(__iomem void *base, unsigned offset){
	__iomem void *reg = base + (offset * 2);
	u32 temp = readw_relaxed(reg + 4) << 16;
	temp |= readw_relaxed(reg);
	return temp;
}

static inline void riu_writel(__iomem void *base, unsigned offset, u32 value){
	__iomem void *reg = base + (offset * 2);
	writew_relaxed(value & 0xffff, reg);
	writew_relaxed(value >> 16, reg + 4);
}

#endif
