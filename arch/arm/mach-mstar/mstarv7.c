// SPDX-License-Identifier: GPL-2.0
/*
 * Device Tree support for MStar/Sigmastar Armv7 SoCs
 *
 * Copyright (c) 2020 thingy.jp
 * Author: Daniel Palmer <daniel@thingy.jp>
 */

#include <linux/init.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/io.h>

/*
 * In the u-boot code the area these registers are in is
 * called "L3 bridge" and there are register descriptions
 * for something in the same area called "AXI".
 *
 * It's not exactly known what this is but the vendor code
 * for both u-boot and linux share calls to "flush the miu pipe".
 * This seems to be to force pending CPU writes to memory so that
 * the state is right before DMA capable devices try to read
 * descriptors and data the CPU has prepared. Without doing this
 * ethernet doesn't work reliably for example.
 */

#define MSTARV7_L3BRIDGE_FLUSH		0x14
#define MSTARV7_L3BRIDGE_STATUS		0x40
#define MSTARV7_L3BRIDGE_FLUSH_TRIGGER	BIT(0)
#define MSTARV7_L3BRIDGE_STATUS_DONE	BIT(12)

#ifdef CONFIG_SMP
#define MSTARV7_CPU1_BOOT_ADDR_HIGH	0x4c
#define MSTARV7_CPU1_BOOT_ADDR_LOW	0x50
#define MSTARV7_CPU1_UNLOCK		0x58
#define MSTARV7_CPU1_UNLOCK_MAGIC	0xbabe
#endif

static void __iomem *l3bridge;

static const char * const mstarv7_board_dt_compat[] __initconst = {
	"mstar,infinity",
	"mstar,infinity2",
	"mstar,infinity2m",
	"mstar,infinity3",
	"mstar,infinity5",
	"mstar,infinity6",
	"mstar,infinity6b0",
	"mstar,infinity6e",
	"mstar,mercury5",
	"sstar,pioneer3",
	NULL,
};

/*
 * This may need locking to deal with situations where an interrupt
 * happens while we are in here and mb() gets called by the interrupt handler.
 *
 * The vendor code did have a spin lock but it doesn't seem to be needed and
 * removing it hasn't caused any side effects so far.
 *
 * [writel|readl]_relaxed have to be used here because otherwise
 * we'd end up right back in here.
 */
static void mstarv7_mb(void)
{
	/* toggle the flush miu pipe fire bit */
	writel_relaxed(0, l3bridge + MSTARV7_L3BRIDGE_FLUSH);
	writel_relaxed(MSTARV7_L3BRIDGE_FLUSH_TRIGGER, l3bridge
			+ MSTARV7_L3BRIDGE_FLUSH);
	while (!(readl_relaxed(l3bridge + MSTARV7_L3BRIDGE_STATUS)
			& MSTARV7_L3BRIDGE_STATUS_DONE)) {
		/* wait for flush to complete */
	}
}

#ifdef CONFIG_SMP
static void __iomem *mstarv7_smpctrl;

static int mstarv7_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	struct device_node *np;
	u32 bootaddr = (u32) __pa_symbol(secondary_startup_arm);
	void __iomem *smpctrl;

	/*
	 * right now we don't know how to boot anything except
	 * cpu 1.
	 */
	if (cpu != 1)
		return -EINVAL;

	np = of_find_compatible_node(NULL, NULL, "mstar,smpctrl");
	smpctrl = of_iomap(np, 0);

	if (!smpctrl)
		return -ENODEV;
#ifdef CONFIG_HOTPLUG_CPU
	if (!mstarv7_smpctrl)
		mstarv7_smpctrl = of_iomap(np, 0);
#endif

	/* set the boot address for the second cpu */
	writew(bootaddr & 0xffff, smpctrl + MSTARV7_CPU1_BOOT_ADDR_LOW);
	writew((bootaddr >> 16) & 0xffff, smpctrl + MSTARV7_CPU1_BOOT_ADDR_HIGH);

	/* unlock the second cpu */
	writew(MSTARV7_CPU1_UNLOCK_MAGIC, smpctrl + MSTARV7_CPU1_UNLOCK);

	/* and away we go...*/
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	iounmap(smpctrl);

	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * There is no power control over the second core, so it parks in WFI
 * until boot_secondary() writes the unlock magic again. Returning from
 * here makes the core re-enter secondary_start_kernel(). After a soft
 * reset (suspend to RAM) the core is in the boot ROM's wait loop instead,
 * and the same unlock takes it through secondary_startup.
 */
static void mstarv7_cpu_die(unsigned int cpu)
{
	void __iomem *smpctrl = mstarv7_smpctrl;

	/* this runs with interrupts off on the dying core, no mapping here */
	if (!smpctrl) {
		for (;;)
			wfi();
	}

	writew(0, smpctrl + MSTARV7_CPU1_UNLOCK);
	do {
		wfi();
	} while (readw(smpctrl + MSTARV7_CPU1_UNLOCK) != MSTARV7_CPU1_UNLOCK_MAGIC);
}

static int mstarv7_cpu_kill(unsigned int cpu)
{
	return 1;
}
#endif

static const struct smp_operations __initdata mstarv7_smp_ops = {
	.smp_boot_secondary = mstarv7_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die	= mstarv7_cpu_die,
	.cpu_kill	= mstarv7_cpu_kill,
#endif
};
#endif

#if defined(CONFIG_SUSPEND) & defined(CONFIG_MACH_INFINITY)
int __init msc313_pm_init(void);
#endif

static void __init mstarv7_init(void)
{
	struct device_node *np;

	of_platform_default_populate(NULL, NULL, NULL);

	np = of_find_compatible_node(NULL, NULL, "mstar,l3bridge");
	l3bridge = of_iomap(np, 0);
	if (l3bridge)
		soc_mb = mstarv7_mb;
	else
		pr_warn("Failed to install memory barrier, DMA will be broken!\n");

#if defined(CONFIG_SUSPEND) & defined(CONFIG_MACH_INFINITY)
        msc313_pm_init();
#endif
}

DT_MACHINE_START(MSTARV7_DT, "MStar/Sigmastar Armv7 (Device Tree)")
	.dt_compat	= mstarv7_board_dt_compat,
	.init_machine	= mstarv7_init,
	.smp		= smp_ops(mstarv7_smp_ops),
MACHINE_END
