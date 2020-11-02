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

#include "clk-mstar-pll_common.h"

/*
 * 0x0  - ??
 * write 0x00c0 - enable
 * write 0x01b2 - disable
 *
 * 0x1c - ??
 *         1         |        0
 * set when disabled | set when enabled
 */

#define REG_MAGIC	0x0
#define REG_ENABLED	0x1c

static const struct of_device_id msc313_upll_of_match[] = {
	{
		.compatible = "mstar,msc313-upll",
	},
	{}
};

static int msc313_upll_is_enabled(struct clk_hw *hw){
	struct mstar_pll_output *output = to_pll_output(hw);
	return ioread16(output->pll->base + REG_ENABLED) & BIT(0);
}

static unsigned long msc313_upll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate){
	struct mstar_pll_output *output = to_pll_output(hw);
	return output->rate;
}

static const struct clk_ops msc313_upll_ops = {
		.is_enabled = msc313_upll_is_enabled,
		.recalc_rate = msc313_upll_recalc_rate,
};

static int msc313_upll_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct mstar_pll *pll;
	int ret = 0;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(msc313_upll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	ret = mstar_pll_common_probe(pdev, &pll, &msc313_upll_ops);
	if(ret)
		goto out;

	iowrite16(0x00c0, pll->base + REG_MAGIC);
	iowrite8(0x01, pll->base + REG_ENABLED);

	platform_set_drvdata(pdev, pll);
out:
	return ret;
}

static int msc313_upll_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct platform_driver msc313_upll_driver = {
	.driver = {
		.name = "msc313-upll",
		.of_match_table = msc313_upll_of_match,
	},
	.probe = msc313_upll_probe,
	.remove = msc313_upll_remove,
};
builtin_platform_driver(msc313_upll_driver);
