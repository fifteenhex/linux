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

#include "clk-mstar-pll_common.h"

static const struct of_device_id mstar_mpll_of_match[] = {
	{
		.compatible = "mstar,mpll",
	},
	{}
};
MODULE_DEVICE_TABLE(of, mstar_mpll_of_match);

static int mstar_mpll_enable(struct clk_hw *hw)
{
	struct mstar_pll_output *output = to_pll_output(hw);
	return 0;
}

static void mstar_mpll_disable(struct clk_hw *hw)
{
	struct mstar_pll_output *output = to_pll_output(hw);
}

static int mstar_mpll_is_enabled(struct clk_hw *hw)
{
	struct mstar_pll_output *output = to_pll_output(hw);
	return 1;
}

static unsigned long mstar_mpll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct mstar_pll_output *output = to_pll_output(hw);
	return output->rate;
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
	struct mstar_pll *pll;
	int ret = 0;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(mstar_mpll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	ret = mstar_pll_common_probe(pdev, &pll, &mstar_mpll_ops);
	if(ret)
		goto out;


	platform_set_drvdata(pdev, pll);
out:
	return ret;
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
