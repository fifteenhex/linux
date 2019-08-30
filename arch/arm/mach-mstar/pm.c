// SPDX-License-Identifier: GPL-2.0
/*
 */

#include <linux/suspend.h>
#include <linux/io.h>
#include <asm/suspend.h>
#include <asm/fncpy.h>
#include <asm/cacheflush.h>

#define MSTARV7_RESUMEADDR		0x1f001cec
#define MSTARV7_RESUMEADDR_SZ		8

extern void msc313_suspend_imi(void);
static void (*msc313_suspend_imi_fn)(void);
static void __iomem *suspend_imi_vbase;

static int msc313_suspend_ready(unsigned long ret)
{
    msc313_suspend_imi_fn = fncpy(suspend_imi_vbase, (void*)&msc313_suspend_imi, 0x1000);

    //flush cache to ensure memory is updated before self-refresh
    __cpuc_flush_kern_all();
    //flush tlb to ensure following translation is all in tlb
    local_flush_tlb_all();
    msc313_suspend_imi_fn();
    return 0;
}

static int msc313_suspend_enter(suspend_state_t state)
{
	switch (state)
	{
		case PM_SUSPEND_MEM:
			cpu_suspend(0, msc313_suspend_ready);
            	break;
        	default:
			return -EINVAL;
	}
    return 0;
}

static void msc313_suspend_finish(void)
{
}

/* sequence: begin, prepare, prepare_late, enter, wake, finish, end */
struct platform_suspend_ops msc313_suspend_ops = {
	.enter    = msc313_suspend_enter,
	.valid    = suspend_valid_only_mem,
	.finish   = msc313_suspend_finish,
};


int __init msc313_pm_init(void)
{
    unsigned int resume_pbase = virt_to_phys(cpu_resume);
    u32* resumeaddr = ioremap(MSTARV7_RESUMEADDR, MSTARV7_RESUMEADDR_SZ);

    suspend_imi_vbase = __arm_ioremap_exec(0xA0010000, 0x1000, false);  //put suspend code at IMI offset 64K;

    suspend_set_ops(&msc313_suspend_ops);

    writel_relaxed(resume_pbase & 0xffff, resumeaddr);
    writel_relaxed((resume_pbase >> 16) & 0xffff, resumeaddr + 1);

    iounmap(resumeaddr);
    return 0;
}
