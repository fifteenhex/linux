/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _SOC_MSTAR_XIU_H_
#define _SOC_MSTAR_XIU_H_

#include <linux/io.h>

static inline u32 xiu_readl(__iomem void *base, unsigned offset){
	__iomem void *reg = base + (offset * 2);
	return readl_relaxed(reg);
}

static inline void xiu_writel(__iomem void *base, unsigned offset, u32 value){
	__iomem void *reg = base + (offset * 2);
	writel_relaxed(value, reg);
}

#endif
