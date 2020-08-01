/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _SOC_MSTAR_RIU_H_
#define _SOC_MSTAR_RIU_H_

#include <linux/io.h>

static inline u32 riu_readl_relaxed(__iomem void *addr){
	u32 temp = readw_relaxed(addr + 4) << 16;
	temp |= readw_relaxed(addr);
	return temp;
}

#endif
