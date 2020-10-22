// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Daniel Palmer
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of_address.h>
#include <linux/module.h>

/*
	miupll_freq = 24 * INREGMSK16(iMiupllBankAddr + REG_ID_03, 0x00FF) / 
	22 * 24
	((INREGMSK16(iMiupllBankAddr + REG_ID_03, 0x0700) >> 8) + 2);
	2 + 2 = 4
	216
*/

#define REG_RATE 0xc

struct mstar_miupll {
	void __iomem *base;
	struct clk_hw clk_hw;
	u32 rate;
};

#define to_miupll(_hw) container_of(_hw, struct mstar_miupll, clk_hw)

static const struct of_device_id mstar_miupll_of_match[] = {
	{
		.compatible = "mstar,miupll",
	},
	{}
};
MODULE_DEVICE_TABLE(of, mstar_miupll_of_match);

static int mstar_miupll_is_enabled(struct clk_hw *hw){
	struct mstar_miupll *miupll = to_miupll(hw);
	return 0;
}

static unsigned long mstar_miupll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate){
	struct mstar_miupll *miupll = to_miupll(hw);
	uint16_t temp = readw_relaxed(miupll->base + REG_RATE);
	unsigned long freq = parent_rate;
	freq *= temp & 0xff;
	freq /= ((temp >> 8) & GENMASK(2, 0)) + 2;
	return freq;
}

static const struct clk_ops mstar_miupll_ops = {
		.is_enabled = mstar_miupll_is_enabled,
		.recalc_rate = mstar_miupll_recalc_rate,
};

static int mstar_miupll_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct mstar_miupll* miupll;
	struct clk_init_data *clk_init;
	struct clk* clk;
	struct resource *mem;
	const char *parents[1];
	int numparents;
	u16 regval;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(mstar_miupll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	miupll = devm_kzalloc(&pdev->dev, sizeof(*miupll), GFP_KERNEL);
	if(!miupll)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	miupll->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(miupll->base))
		return PTR_ERR(miupll->base);

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, 1);
	if(numparents <= 0)
	{
		dev_info(&pdev->dev, "need some parents");
		return -EINVAL;
	}

	clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
	if(!clk_init)
		return -ENOMEM;

	miupll->clk_hw.init = clk_init;
	clk_init->name = pdev->dev.of_node->name;
	clk_init->ops = &mstar_miupll_ops;
	clk_init->num_parents = numparents;
	clk_init->parent_names = parents;

	clk = clk_register(&pdev->dev, &miupll->clk_hw);
	if(IS_ERR(clk)){
		printk("failed to register clk");
		return -ENOMEM;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get, clk);
}

static int mstar_miupll_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mstar_miupll_driver = {
	.driver = {
		.name = "mstar-miupll",
		.of_match_table = mstar_miupll_of_match,
	},
	.probe = mstar_miupll_probe,
	.remove = mstar_miupll_remove,
};
module_platform_driver(mstar_miupll_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar miupll clock driver");
