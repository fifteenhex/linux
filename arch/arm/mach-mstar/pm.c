// SPDX-License-Identifier: GPL-2.0
/*
 */

#include <asm/suspend.h>
#include <asm/fncpy.h>
#include <asm/cacheflush.h>

#include <linux/suspend.h>
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/genalloc.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include <soc/mstar/pmsleep.h>
#include <soc/mstar/str.h>

#define COMPAT_PMSLEEP	"mstar,msc313-pmsleep"
#define COMPAT_MIU	"mstar,msc313-miu"

#define MSTARV7_PM_SIZE			SZ_16K
#define MSTARV7_PM_INFO_OFFSET		0
#define MSTARV7_PM_INFO_SIZE		SZ_4K
#define MSTARV7_PM_SUSPEND_OFFSET	(MSTARV7_PM_INFO_OFFSET + MSTARV7_PM_INFO_SIZE)
#define MSTARV7_PM_SUSPEND_SIZE		SZ_4K

struct mstar_pm_info {
	u32 pmsleep;	// 0x0
	u32 pmgpio;	// 0x4
	u32 miu_ana;	// 0x8
	u32 miu_dig;	// 0xc
	u32 miu_dig1;	// 0x10
	u32 pmuart;	// 0x14
	u32 bank1020;	// 0x18, the bank with the cpu clock switch
	u32 cpupll;	// 0x1c
	u32 mpll;	// 0x20
	u32 flags;	// 0x24
};

/* mstar_pm_keep_clocks: 1 keeps the CPU on its PLL, 2 keeps the MPLL on in sleep (debug) */
static unsigned int keep_clocks;
core_param(mstar_pm_keep_clocks, keep_clocks, uint, 0644);

static struct mstar_pm_info __iomem *pm_info;
/* the mapped banks, copied into the SRAM info block before each sleep */
static struct mstar_pm_info pm_bases;

static void mstar_pm_fill_info(void)
{
	memcpy_toio(pm_info, &pm_bases, sizeof(pm_bases));
}
static void __iomem *pm_suspend_code;

static struct regmap *pmsleep;
/* the GIC CPU interface, for after the resume */
static void __iomem *gicc;
#define GIC_CPU_CTRL		0x00
#define GICC_BYPASS_DISABLE	0x1e0

extern void msc313_suspend_imi(struct mstar_pm_info *pm_info);
extern void msc313_resume_imi(void);
extern void ssd20xd_suspend_imi(struct mstar_pm_info *pm_info);

/* The soft reset style suspend (ssd20xd) leaves this for the first stage loader */
static struct mstar_str_info __iomem *str_info;
static bool soft_reset_suspend;

static void mstar_str_set_status(u32 status)
{
	u32 magic = MSTAR_STR_MAGIC;
	u32 resume = virt_to_phys(cpu_resume);

	if (!str_info)
		return;

	writel(magic, &str_info->magic);
	writel(resume, &str_info->resume);
	writel(status, &str_info->status);
	writel(mstar_str_check(magic, resume, status), &str_info->check);
}
static void (*msc313_suspend_imi_fn)(struct mstar_pm_info *pm_info);

static int msc313_suspend_ready(unsigned long ret)
{
	local_flush_tlb_all();
	flush_cache_all();
	msc313_suspend_imi_fn(pm_info);
	return 0;
}

static int msc313_suspend_enter(suspend_state_t state)
{
	switch (state){
	case PM_SUSPEND_MEM:
		if (soft_reset_suspend) {
			/*
			 * The first stage loader uses the SRAM after the reset, so
			 * put the sleep code back in place every time.
			 */
			msc313_suspend_imi_fn = fncpy(pm_suspend_code,
					(void*)&ssd20xd_suspend_imi, MSTARV7_PM_SUSPEND_SIZE);
			mstar_pm_fill_info();
			mstar_str_set_status(MSTAR_STR_STATUS_ASLEEP);
			pm_info->flags = keep_clocks;
		} else
			regmap_update_bits(pmsleep, MSTAR_PMSLEEP_REG24,
				MSTAR_PMSLEEP_REG24_POWEROFF, MSTAR_PMSLEEP_REG24_POWEROFF);
		/* the GIC and the CPU timers save their state on these */
		cpu_pm_enter();
		cpu_cluster_pm_enter();
		cpu_suspend(0, msc313_suspend_ready);
		/*
		 * We are back through a reset. The bootloader normally sets the
		 * GIC CPU interface to keep the legacy IRQ and FIQ lines from
		 * bypassing the GIC; the GIC driver keeps those bits when it
		 * brings the interface up, so put them back before it does.
		 */
		if (soft_reset_suspend && gicc)
			writel(GICC_BYPASS_DISABLE, gicc + GIC_CPU_CTRL);
		cpu_cluster_pm_exit();
		cpu_pm_exit();
		mstar_str_set_status(MSTAR_STR_STATUS_AWAKE);
	break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void msc313_suspend_finish(void)
{
	/* aborted or done, either way the loader must boot normally next time */
	mstar_str_set_status(MSTAR_STR_STATUS_AWAKE);
}

/* sequence: begin, prepare, prepare_late, enter, wake, finish, end */
static const struct platform_suspend_ops msc313_suspend_ops = {
	.enter    = msc313_suspend_enter,
	.valid    = suspend_valid_only_mem,
	.finish   = msc313_suspend_finish,
};

static void mstar_poweroff(void)
{
	regmap_update_bits(pmsleep, MSTAR_PMSLEEP_REG24,
			MSTAR_PMSLEEP_REG24_POWEROFF, ~0);
	msc313_suspend_imi_fn(pm_info);
}

int __init msc313_pm_init(void)
{
	int ret = 0;
	struct device_node *node;
	struct platform_device *pdev;
	struct gen_pool *imi_pool;
	unsigned long imi_base;
	void __iomem *virt;
	phys_addr_t phys;
	unsigned int resume_pbase;

	pmsleep = syscon_regmap_lookup_by_compatible(COMPAT_PMSLEEP);
	if(!pmsleep)
		return -ENODEV;

	node = of_find_compatible_node(NULL, NULL, "mmio-sram");
	if (!node) {
		pr_warn("%s: failed to find imi node\n", __func__);
		return -ENODEV;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		pr_warn("%s: failed to find imi device\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	imi_pool = gen_pool_get(&pdev->dev, NULL);
	if (!imi_pool) {
		pr_warn("%s: imi pool unavailable!\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	imi_base = gen_pool_alloc(imi_pool, MSTARV7_PM_SIZE);
	if (!imi_base) {
		pr_warn("%s: unable to alloc pm memory in imi!\n", __func__);
		ret = -ENOMEM;
		goto put_node;
	}

	phys = gen_pool_virt_to_phys(imi_pool, imi_base);
	virt = __arm_ioremap_exec(phys, MSTARV7_PM_SIZE, false);
	pm_info = (struct mstar_pm_info*) (virt + MSTARV7_PM_INFO_OFFSET);
	pm_suspend_code = virt + MSTARV7_PM_SUSPEND_OFFSET;

	node = of_find_compatible_node(NULL, NULL, COMPAT_PMSLEEP);
	pm_bases.pmsleep = (u32) of_iomap(node, 0);
	pm_bases.pmgpio	= (u32) ioremap(0x1f001e00, 0x200);

	node = of_find_compatible_node(NULL, NULL, COMPAT_MIU);
	pm_bases.miu_ana  = (u32) of_iomap(node, 0);
	pm_bases.miu_dig  = (u32) of_iomap(node, 1);
	pm_bases.miu_dig1 = (u32) of_iomap(node, 2);
	pm_bases.pmuart	  = (u32) ioremap(0x1f221000, 0x200);

	soft_reset_suspend = of_machine_is_compatible("mstar,infinity2m");
	if (soft_reset_suspend) {
		pm_bases.bank1020 = (u32) ioremap(0x1f204000, 0x200);
		pm_bases.cpupll = (u32) ioremap(0x1f206400, 0x200);
		pm_bases.mpll = (u32) ioremap(0x1f206000, 0x200);
		str_info = ioremap(MSTAR_STR_INFO_PHYS, SZ_256);
		gicc = ioremap(0x16002000, SZ_4K);
		if (!str_info) {
			pr_warn("%s: cannot map the suspend handshake\n", __func__);
			ret = -ENOMEM;
			goto put_node;
		}
		mstar_str_set_status(MSTAR_STR_STATUS_AWAKE);
		msc313_suspend_imi_fn = fncpy(pm_suspend_code,
				(void*)&ssd20xd_suspend_imi, MSTARV7_PM_SUSPEND_SIZE);
	} else {
		msc313_suspend_imi_fn = fncpy(pm_suspend_code,
				(void*)&msc313_suspend_imi, MSTARV7_PM_SUSPEND_SIZE);
	}
	mstar_pm_fill_info();

	/* setup the resume addr for the bootrom */
	resume_pbase = __pa_symbol(msc313_resume_imi);
	regmap_write(pmsleep, MSTARV7_PM_RESUMEADDR, resume_pbase & 0xffff);
	regmap_write(pmsleep, MSTARV7_PM_RESUMEADDR + 4, (resume_pbase >> 16) & 0xffff);

	suspend_set_ops(&msc313_suspend_ops);

	pm_power_off = mstar_poweroff;

	printk("pm code is at %px, pm info is at %px, pmsleep is at %x, pmgpio is at %x, %s\n",
			pm_suspend_code, pm_info, pm_info->pmsleep, pm_info->pmgpio,
			soft_reset_suspend ? "sleep ends in a soft reset" : "sleep powers off");

put_node:
	of_node_put(node);

	return ret;
}
