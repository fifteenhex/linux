// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 *
 * 0x100 - 15 |   13    |       12     |     11     |   10    |     9     |     8      |     3      | 2 - 0
 *         pd | en_mini | fifo_div5_en | dual_lp_en | en_fifo | en_scalar | sdiv2p5_en | sdiv3p5_en | ictrl
 *
 * 0x104 -    11 - 8    |     5 - 4    |     1 - 0
 *         loop_div_sec | loop_div_fst | input_div_fst
 *
 * 0x108 -      10 - 8  |       7 - 4    |     1 - 0
 *             fifo_div | scalar_div_sec | scalar_div_fst
 *
 * 0x10c -     4        |  2 - 0
 *       skew_en_fixclk | skew_div
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of_address.h>
#include <linux/module.h>

struct msc313_upll {
	__iomem void *base;
	struct clk_hw clk_hw;
	u32 rate;
};

#define to_upll(_hw) container_of(_hw, struct msc313_upll, clk_hw)

static const struct of_device_id mstar_lpll_of_match[] = {
	{
		.compatible = "mstar,lpll",
	},
	{}
};
MODULE_DEVICE_TABLE(of, mstar_lpll_of_match);

static int mstar_lpll_is_enabled(struct clk_hw *hw){
	struct msc313_upll *upll = to_upll(hw);
	return 0;
}

static unsigned long mstar_lpll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate){
	struct msc313_upll *upll = to_upll(hw);
	return 0;
}

static const struct clk_ops mstar_lpll_ops = {
	.is_enabled = mstar_lpll_is_enabled,
	.recalc_rate = mstar_lpll_recalc_rate,
};

static int mstar_lpll_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct msc313_upll* upll;
	struct clk_init_data *clk_init;
	struct clk* clk;
	struct resource *mem;
	const char *parents[16];
	int numparents;
	u16 regval;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(mstar_lpll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	upll = devm_kzalloc(&pdev->dev, sizeof(*upll), GFP_KERNEL);
	if(!upll)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	upll->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(upll->base))
		return PTR_ERR(upll->base);

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, 16);
	if(numparents <= 0)
	{
		dev_info(&pdev->dev, "need some parents");
		return -EINVAL;
	}

	clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
	if(!clk_init)
		return -ENOMEM;

	upll->clk_hw.init = clk_init;
	clk_init->name = pdev->dev.of_node->name;
	clk_init->ops = &mstar_lpll_ops;
	clk_init->num_parents = numparents;
	clk_init->parent_names = parents;

	clk = clk_register(&pdev->dev, &upll->clk_hw);
	if(IS_ERR(clk)){
		printk("failed to register clk");
		return -ENOMEM;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get, clk);
}

static int mstar_lpll_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mstar_lpll_driver = {
	.driver = {
		.name = "mstar-lpll",
		.of_match_table = mstar_lpll_of_match,
	},
	.probe = mstar_lpll_probe,
	.remove = mstar_lpll_remove,
};
module_platform_driver(mstar_lpll_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar lpll clock driver");
