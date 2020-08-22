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
#include <linux/regmap.h>

#define REG_LOCK	0x0
#define REG_LOCK_OFF	BIT(1)
#define REG_FORCEON	0x4
#define REG_FORCEOFF	0x8
#define REG_ENRD	0xc

#define MAX_OUTPUTS	16

struct msc313_pll_gate {
	struct regmap* regmap;
	struct clk_hw clk_hw;
	u16 mask;
};

#define to_pll_gate(_hw) container_of(_hw, struct msc313_pll_gate, clk_hw)

static int msc313_pll_gater_enable(struct clk_hw *hw)
{
	struct msc313_pll_gate *pll_gate = to_pll_gate(hw);

	regmap_write_bits(pll_gate->regmap, REG_FORCEON, pll_gate->mask, pll_gate->mask);

	return 0;
}

static void msc313_pll_gater_disable(struct clk_hw *hw)
{
	struct msc313_pll_gate *pll_gate = to_pll_gate(hw);

	/* never force a clock off */
	regmap_write_bits(pll_gate->regmap, REG_FORCEON, pll_gate->mask, 0);
}

static int msc313_pll_gater_is_enabled(struct clk_hw *hw)
{
	struct msc313_pll_gate *pll_gate = to_pll_gate(hw);
	unsigned int val;
	int ret;

	ret = regmap_read(pll_gate->regmap, REG_ENRD, &val);

	return (val & pll_gate->mask) ? 1 : 0;
}

static unsigned long msc313_pll_gater_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	return parent_rate;
}

static const struct clk_ops msc313_pll_gater_ops = {
		.enable = msc313_pll_gater_enable,
		.disable = msc313_pll_gater_disable,
		.is_enabled = msc313_pll_gater_is_enabled,
		.recalc_rate = msc313_pll_gater_recalc_rate,
};

static const struct regmap_config msc313_pll_regmap_config = {
                .reg_bits = 16,
                .val_bits = 16,
                .reg_stride = 4,
};

static int msc313_pll_gater_probe(struct platform_device *pdev)
{
	struct msc313_pll_gate* pll_gate;
	struct clk_init_data *clk_init;
	struct clk* clk;
	void __iomem *base;
	int numparents, numoutputs, pllindex;
	struct clk_onecell_data *clk_data;
	const char *parents[16];
	struct regmap* regmap;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, 16);
	if(!numparents){
		dev_err(&pdev->dev, "no parents\n");
		return -ENODEV;
	}

	if(numparents != of_clk_get_parent_count(pdev->dev.of_node)){
		dev_info(&pdev->dev, "waiting for parents\n");
		return -EPROBE_DEFER;
	}

	numoutputs = of_property_count_strings(pdev->dev.of_node, "clock-output-names");
	if(!numoutputs){
		dev_info(&pdev->dev, "output names need to be specified");
		return -ENODEV;
	}

	if(numoutputs > MAX_OUTPUTS){
		dev_info(&pdev->dev, "too many output names");
		return -EINVAL;
	}

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base, &msc313_pll_regmap_config);
	if(IS_ERR(regmap)){
		dev_err(&pdev->dev, "failed to register regmap");
		return PTR_ERR(regmap);
	}

	// Clear the force on register so we can actually control the gates
	regmap_write(regmap, REG_FORCEON, 0x0);
	// Clear the force off register
	regmap_write(regmap, REG_FORCEOFF, 0x0);
	// lock the force off bits
	regmap_write(regmap, REG_LOCK, REG_LOCK_OFF);

	clk_data = devm_kzalloc(&pdev->dev, sizeof(struct clk_onecell_data), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->clk_num = numoutputs;
	clk_data->clks = devm_kcalloc(&pdev->dev, numoutputs, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		return -ENOMEM;

	pll_gate = devm_kcalloc(&pdev->dev, numoutputs, sizeof(*pll_gate), GFP_KERNEL);
	if (!pll_gate)
		return -ENOMEM;

	clk_init = devm_kcalloc(&pdev->dev, numoutputs, sizeof(*clk_init), GFP_KERNEL);
	if (!clk_init)
		return -ENOMEM;

	for (pllindex = 0; pllindex < numoutputs; pllindex++, pll_gate++, clk_init++) {
		pll_gate->regmap = regmap;
		pll_gate->mask = 1 << pllindex;
		pll_gate->clk_hw.init = clk_init;

		of_property_read_string_index(pdev->dev.of_node,
				"clock-output-names", pllindex, &clk_init->name);
		clk_init->ops = &msc313_pll_gater_ops;

		clk_init->num_parents = 1;
		clk_init->parent_names = parents + pllindex;

		clk = devm_clk_register(&pdev->dev, &pll_gate->clk_hw);
		if (IS_ERR(clk)) {
			printk("failed to register clk");
			return -ENOMEM;
		}
		clk_data->clks[pllindex] = clk;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);
}

static int msc313_pll_gater_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id msc313_pll_gater_of_match[] = {
	{ .compatible = "mstar,msc313-pll-gater", },
	{}
};
MODULE_DEVICE_TABLE(of, msc313_pll_gater_of_match);

static struct platform_driver msc313_pll_gater_driver = {
	.driver = {
		.name = "msc313-pll-gater",
		.of_match_table = msc313_pll_gater_of_match,
	},
	.probe = msc313_pll_gater_probe,
	.remove = msc313_pll_gater_remove,
};
builtin_platform_driver(msc313_pll_gater_driver);
