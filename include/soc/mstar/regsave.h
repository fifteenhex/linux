/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Suspend to RAM on these chips ends in a soft reset, so every block comes
 * back with its registers at their reset values while the drivers still
 * believe what they programmed. This keeps a copy of a bank of 16 bit
 * registers (on the RIU they sit 4 bytes apart) across the sleep.
 */
#ifndef _SOC_MSTAR_REGSAVE_H_
#define _SOC_MSTAR_REGSAVE_H_

#include <linux/device.h>
#include <linux/io.h>
#include <linux/slab.h>

struct mstar_regsave {
	void __iomem *base;
	unsigned int count;
	u16 *vals;
};

static inline int mstar_regsave_init(struct device *dev, struct mstar_regsave *rs,
				     void __iomem *base, unsigned int bytes)
{
	rs->base = base;
	rs->count = bytes / 4;
	rs->vals = devm_kcalloc(dev, rs->count, sizeof(*rs->vals), GFP_KERNEL);
	return rs->vals ? 0 : -ENOMEM;
}

static inline void mstar_regsave_save(struct mstar_regsave *rs)
{
	unsigned int i;

	for (i = 0; i < rs->count; i++)
		rs->vals[i] = readw_relaxed(rs->base + i * 4);
}

static inline void mstar_regsave_restore(struct mstar_regsave *rs)
{
	unsigned int i;

	for (i = 0; i < rs->count; i++)
		writew_relaxed(rs->vals[i], rs->base + i * 4);
}

#endif
