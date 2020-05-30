// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of_address.h>
#include <linux/module.h>
#include <linux/kernel.h>


/*
 * CPU clock seems to come from a PLL that has registers at 0x1f206500
 * 0x40 -- LPF low. Seems to store one half of the clock transition
 * 0x44 /
 * 0x48 -- LPF high. Seems to store one half of the clock transition
 * 0x4c /
 * 0x50 -- code says "toggle lpf enable"
 * 0x54 -- mu?
 * 0x5c -- lpf_update_count?
 * 0x60 -- code says "switch to LPF". Clock source config? Register bank?
 * 0x64 -- code says "from low to high" which seems to mean transition from LPF low to LPF high.
 * 0x74 -- Seems to be the PLL lock status bit
 *
 * 0x80 -- Seems to be the current frequency, this might need to be populated by software?
 * 0x84 /  The vendor driver uses these to set the initial value of LPF low
 *
 * frequency seems to be calculated like this:
 * (parent clock (432mhz) / register_magic_value) * 12 * 524288
 *
 * Vendor values:
 *
 * frequency - register value
 *
 * 400000000  - 0x0067AE14
 * 600000000  - 0x00451EB8,
 * 800000000  - 0x0033D70A,
 * 1000000000 - 0x002978d4,
 */

struct msc313e_cpuclk {
	void __iomem *base;
	struct clk_hw clk_hw;
};

#define to_cpuclk(_hw) container_of(_hw, struct msc313e_cpuclk, clk_hw)

struct freqregisters {
	u32 frequency;
	u16 bottom, top;
};

#define REG_LPF_LOW_L		0x140
#define REG_LPF_LOW_H		0x144
#define REG_LPF_HIGH_BOTTOM	0x148
#define REG_LPF_HIGH_TOP	0x14c
#define REG_LPF_TOGGLE		0x150
#define REG_LPF_MYSTERYTWO	0x154
#define REG_LPF_UPDATE_COUNT	0x15c
#define REG_LPF_MYSTERYONE	0x160
#define REG_LPF_TRANSITIONCTRL	0x164
#define REG_LPF_LOCK		0x174
#define REG_CURRENT		0x180

static u32 msc313_cpuclk_reg_read32(struct msc313e_cpuclk *cpuclk, unsigned reg){
	u32 value = ioread16(cpuclk->base + reg + 4);
	value = value << 16;
	value |= ioread16(cpuclk->base + reg);
	return value;
}

static void msc313_cpuclk_reg_write32(struct msc313e_cpuclk *cpuclk, unsigned reg, u32 value){
	u16 l = value & 0xffff, h = (value >> 16) & 0xffff;
	iowrite16(l, cpuclk->base + reg);
	iowrite16(h, cpuclk->base + reg + 4);
}

static void msc313_cpuclk_setfreq(struct msc313e_cpuclk *cpuclk, u32 reg) {
	msc313_cpuclk_reg_write32(cpuclk, REG_LPF_HIGH_BOTTOM, reg);

	iowrite16(0x1, cpuclk->base + REG_LPF_MYSTERYONE);
	iowrite16(0x6, cpuclk->base + REG_LPF_MYSTERYTWO);
	iowrite16(0x8, cpuclk->base + REG_LPF_UPDATE_COUNT);
	iowrite16(BIT(12), cpuclk->base + REG_LPF_TRANSITIONCTRL);

	iowrite16(0, cpuclk->base + REG_LPF_TOGGLE);
	iowrite16(1, cpuclk->base + REG_LPF_TOGGLE);

	while(!(ioread16(cpuclk->base + REG_LPF_LOCK)));

	iowrite16(0, cpuclk->base + REG_LPF_TOGGLE);

	msc313_cpuclk_reg_write32(cpuclk, REG_LPF_LOW_L, reg);
}

static unsigned long msc313e_cpuclk_frequencyforreg(u32 reg, unsigned long parent_rate){
	unsigned long long prescaled = ((unsigned long long)parent_rate) * (12 * 524288);
	unsigned long long scaled;
	if(prescaled == 0 || reg == 0)
		return 0;
	scaled = DIV_ROUND_DOWN_ULL(prescaled, reg);
	//printk("%x == %llu from %lu\n", reg, scaled, parent_rate);
	return scaled;
}

static u32 msc313e_cpuclk_regforfrequecy(unsigned long rate, unsigned long parent_rate){
	unsigned long long prescaled = ((unsigned long long)parent_rate) * (12 * 524288);
	unsigned long long scaler;
	u32 reg;
	if(prescaled == 0 || rate == 0)
		return 0;
	scaler = DIV_ROUND_UP_ULL(prescaled, rate);
	reg = scaler;
	//printk("%lu from %lu -> %x\n", rate, parent_rate, reg);
	return reg;
}

static unsigned long msc313e_cpuclk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate){
	struct msc313e_cpuclk *cpuclk = to_cpuclk(hw);
	return msc313e_cpuclk_frequencyforreg(msc313_cpuclk_reg_read32(cpuclk, REG_LPF_LOW_L), parent_rate);
}

static long msc313e_cpuclk_round_rate(struct clk_hw *hw,
			    unsigned long rate,
			    unsigned long *parent_rate)
{
	u32 reg = msc313e_cpuclk_regforfrequecy(rate, *parent_rate);
	long rounded = msc313e_cpuclk_frequencyforreg(reg, *parent_rate);
	// this is my poor attempt at making sure the resulting rate doesn't overshoot the requested rate
	for(; rounded >= rate && reg > 0; reg--){
		rounded = msc313e_cpuclk_frequencyforreg(reg, *parent_rate);
		//printk("rounded %ld\n", rounded);
	}
	return rounded;
}

static int msc313e_cpuclk_set_rate(struct clk_hw *hw,
			 unsigned long rate,
			 unsigned long parent_rate)
{
	struct msc313e_cpuclk *cpuclk = to_cpuclk(hw);
	u32 reg = msc313e_cpuclk_regforfrequecy(rate, parent_rate);
	msc313_cpuclk_setfreq(cpuclk, reg);
	return 0;
}

static const struct clk_ops msc313e_cpuclk_ops = {
		.recalc_rate = msc313e_cpuclk_recalc_rate,
		.round_rate =  msc313e_cpuclk_round_rate,
		.set_rate = msc313e_cpuclk_set_rate
};

static const struct of_device_id msc313e_cpuclk_of_match[] = {
	{
		.compatible = "mstar,msc313e-cpuclk",
	},
	{}
};
MODULE_DEVICE_TABLE(of, msc313e_cpuclk_of_match);

static int msc313e_cpuclk_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct msc313e_cpuclk* cpuclk;
	struct clk_init_data *clk_init;
	struct clk* clk;
	struct resource *mem;
	const char *parents[16];
	int numparents;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(msc313e_cpuclk_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	cpuclk = devm_kzalloc(&pdev->dev, sizeof(*cpuclk), GFP_KERNEL);
	if(!cpuclk)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cpuclk->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(cpuclk->base))
		return PTR_ERR(cpuclk->base);

	/* LPF might not contain the current frequency so fix that up */
	msc313_cpuclk_reg_write32(cpuclk, REG_LPF_LOW_L,
			msc313_cpuclk_reg_read32(cpuclk, REG_CURRENT));

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, 16);
	if(numparents <= 0)
	{
		dev_info(&pdev->dev, "need some parents");
		return -EINVAL;
	}

	clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
	if(!clk_init)
		return -ENOMEM;

	cpuclk->clk_hw.init = clk_init;
	clk_init->name = pdev->dev.of_node->name;
	clk_init->ops = &msc313e_cpuclk_ops;
	clk_init->flags = CLK_IS_CRITICAL;
	clk_init->num_parents = numparents;
	clk_init->parent_names = parents;

	clk = clk_register(&pdev->dev, &cpuclk->clk_hw);
	if(IS_ERR(clk)){
		printk("failed to register clk");
		return -ENOMEM;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get, clk);
}

static int msc313e_cpuclk_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313e_cpuclk_driver = {
	.driver = {
		.name = "msc313e-cpuclk",
		.of_match_table = msc313e_cpuclk_of_match,
	},
	.probe = msc313e_cpuclk_probe,
	.remove = msc313e_cpuclk_remove,
};
module_platform_driver(msc313e_cpuclk_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MSC313e cpu clock driver");
