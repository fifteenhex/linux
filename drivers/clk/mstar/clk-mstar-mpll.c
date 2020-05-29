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

struct mstar_mpll {
	u32 rate;
	struct clk_hw clk_hw;
};

#define to_clkgen_pll(_hw) container_of(_hw, struct mstar_mpll, clk_hw)

static const struct of_device_id mstar_mpll_of_match[] = {
	{
		.compatible = "mstar,mpll",
	},
	{}
};
MODULE_DEVICE_TABLE(of, mstar_mpll_of_match);

static int mstar_mpll_enable(struct clk_hw *hw)
{
	struct mstar_mpll *clkgen_pll = to_clkgen_pll(hw);
	return 0;
}

static void mstar_mpll_disable(struct clk_hw *hw)
{
	struct mstar_mpll *clkgen_pll = to_clkgen_pll(hw);
}

static int mstar_mpll_is_enabled(struct clk_hw *hw)
{
	struct mstar_mpll *clkgen_pll = to_clkgen_pll(hw);
	return 1;
}

static unsigned long mstar_mpll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct mstar_mpll *clkgen_pll = to_clkgen_pll(hw);
	return clkgen_pll->rate;
}

static const struct clk_ops mstar_mpll_ops = {
		.enable = mstar_mpll_enable,
		.disable = mstar_mpll_disable,
		.is_enabled = mstar_mpll_is_enabled,
		.recalc_rate = mstar_mpll_recalc_rate
};

static int mstar_mpll_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct mstar_mpll* clkgen_pll;
	struct clk_init_data *clk_init;
	struct clk* clk;
	int numparents, numoutputs, numrates, pllindex;
	struct clk_onecell_data *clk_data;
	const char *parents[1];

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(mstar_mpll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, ARRAY_SIZE(parents));

	numoutputs = of_property_count_strings(pdev->dev.of_node, "clock-output-names");
	if(!numoutputs){
		dev_info(&pdev->dev, "output names need to be specified");
		return -ENODEV;
	}

	if(numoutputs > 16){
		dev_info(&pdev->dev, "too many output names");
		return -EINVAL;
	}

	numrates = of_property_count_u32_elems(pdev->dev.of_node, "clock-rates");
	if(!numrates){
		dev_info(&pdev->dev, "clock rates need to be specified");
		return -ENODEV;
	}

	if(numrates != numoutputs){
		dev_info(&pdev->dev, "number of clock rates must match the number of outputs");
		return -EINVAL;
	}

	clk_data = devm_kzalloc(&pdev->dev, sizeof(struct clk_onecell_data), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->clk_num = numoutputs;
	clk_data->clks = devm_kcalloc(&pdev->dev, numoutputs, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		return -ENOMEM;

	for (pllindex = 0; pllindex < numoutputs; pllindex++) {
		clkgen_pll = devm_kzalloc(&pdev->dev, sizeof(*clkgen_pll), GFP_KERNEL);
		if (!clkgen_pll)
			return -ENOMEM;

		of_property_read_u32_index(pdev->dev.of_node, "clock-rates",
				pllindex, &clkgen_pll->rate);

		clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
		if (!clk_init)
			return -ENOMEM;

		clkgen_pll->clk_hw.init = clk_init;
		of_property_read_string_index(pdev->dev.of_node,
				"clock-output-names", pllindex, &clk_init->name);
		clk_init->ops = &mstar_mpll_ops;

		clk_init->num_parents = 1;
		clk_init->parent_names = parents;

		clk = clk_register(&pdev->dev, &clkgen_pll->clk_hw);
		if (IS_ERR(clk)) {
			printk("failed to register clk");
			return -ENOMEM;
		}
		clk_data->clks[pllindex] = clk;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);
}

static int mstar_mpll_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mstar_mpll_driver = {
	.driver = {
		.name = "mstar-mpll",
		.of_match_table = mstar_mpll_of_match,
	},
	.probe = mstar_mpll_probe,
	.remove = mstar_mpll_remove,
};
module_platform_driver(mstar_mpll_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MPLL driver");
