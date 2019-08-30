// SPDX-License-Identifier: GPL-2.0
/*
 * Device Tree support for MStar Infinity SoCs
 *
 * Copyright (c) 2019 thingy.jp
 * Author: Daniel Palmer <daniel@thingy.jp>
 */

#include <linux/init.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <linux/of.h>
#include <linux/io.h>

/*
 * In the u-boot code the area these registers are in is
 * called "L3 bridge".
 *
 * It's not exactly known what is the L3 bridge is but
 * the vendor code for both u-boot and linux share calls
 * to "flush the miu pipe". This seems to be to force pending
 * CPU writes to memory so that the state is right before
 * DMA capable devices try to read descriptors and data
 * the CPU has prepared. Without doing this ethernet doesn't
 * work reliably for example.
 */

#define INFINITY_L3BRIDGE_FLUSH		0x1f204414
#define INFINITY_L3BRIDGE_STATUS	0x1f204440
#define INFINITY_L3BRIDGE_FLUSH_TRIGGER	BIT(0)
#define INFINITY_L3BRIDGE_STATUS_DONE	BIT(12)

static u32 __iomem *miu_status;
static u32 __iomem *miu_flush;

static const char * const infinity_board_dt_compat[] = {
	"mstar,infinity",
	"mstar,infinity3",
	"mstar,infinity5",
	"mstar,infinity6",
	"mstar,mercury5",
	NULL,
};

static const char * const infinity_smp_board_dt_compat[] = {
	"mstar,infinity2m",
	NULL,
};

/*
 * This may need locking to deal with situations where an interrupt
 * happens while we are in here and mb() gets called by the interrupt handler.
 */
static void infinity_mb(void)
{
	/* toggle the flush miu pipe fire bit */
	writel_relaxed(0, miu_flush);
	writel_relaxed(INFINITY_L3BRIDGE_FLUSH_TRIGGER, miu_flush);
	while (!(readl_relaxed(miu_status) & INFINITY_L3BRIDGE_STATUS_DONE)) {
		/* wait for flush to complete */
	}
}

static void __init infinity_barriers_init(void)
{
	miu_flush = ioremap(INFINITY_L3BRIDGE_FLUSH, sizeof(miu_flush));
	miu_status = ioremap(INFINITY_L3BRIDGE_STATUS, sizeof(miu_status));
	soc_mb = infinity_mb;
}

#ifdef CONFIG_SUSPEND
int __init msc313_pm_init(void);
#endif

static void __init infinity_init(void)
{
	infinity_barriers_init();
#ifdef CONFIG_SUSPEND
        msc313_pm_init();
#endif
}

DT_MACHINE_START(INFINITY_DT, "MStar Infinity (Device Tree)")
	.dt_compat	= infinity_board_dt_compat,
	.init_machine	= infinity_init,
MACHINE_END

DT_MACHINE_START(INFINITY_SMP_DT, "MStar Infinity SMP (Device Tree)")
	.dt_compat	= infinity_smp_board_dt_compat,
	.init_machine	= infinity_init,
#ifdef CONFIG_SMP
#endif
MACHINE_END
