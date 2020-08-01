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

static DEFINE_SPINLOCK(msc313e_pllgate_lock);

struct msc313e_clkgen_pll {
	u16 mask;
	u32 rate;
	struct clk_hw clk_hw;
	struct regmap* regmap;
};

#define to_clkgen_pll(_hw) container_of(_hw, struct msc313e_clkgen_pll, clk_hw)

static const struct of_device_id msc313e_clkgen_pll_of_match[] = {
	{
		.compatible = "mstar,msc313e-clkgen-pll",
	},
	{}
};
MODULE_DEVICE_TABLE(of, msc313e_clkgen_pll_of_match);

static	int	msc313_clkgen_pll_enable(struct clk_hw *hw)
{
	struct msc313e_clkgen_pll *clkgen_pll = to_clkgen_pll(hw);
	spin_lock(&msc313e_pllgate_lock);
	regmap_write_bits(clkgen_pll->regmap, REG_FORCEON, clkgen_pll->mask, clkgen_pll->mask);
	spin_unlock(&msc313e_pllgate_lock);

	//printk("val was -> %x now -> %x\n", valb, vala);
	return 0;
}

static void	msc313e_clkgen_pll_disable(struct clk_hw *hw)
{
	struct msc313e_clkgen_pll *clkgen_pll = to_clkgen_pll(hw);
	spin_lock(&msc313e_pllgate_lock);
	/* never force a clock off */
	regmap_write_bits(clkgen_pll->regmap, REG_FORCEON, clkgen_pll->mask, 0);
	spin_unlock(&msc313e_pllgate_lock);
}

static int	msc313e_clkgen_pll_is_enabled(struct clk_hw *hw)
{
	struct msc313e_clkgen_pll *clkgen_pll;
	unsigned int val;
	int ret;

	clkgen_pll = to_clkgen_pll(hw);
	spin_lock(&msc313e_pllgate_lock);
	ret = regmap_read(clkgen_pll->regmap, REG_ENRD, &val);
	spin_unlock(&msc313e_pllgate_lock);
	return (val & clkgen_pll->mask) ? 1 : 0;
}

static unsigned long msc313e_clkgen_pll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct msc313e_clkgen_pll *clkgen_pll = to_clkgen_pll(hw);
	return clkgen_pll->rate;
}

static const struct clk_ops msc313e_clkgen_pll_ops = {
		.enable = msc313_clkgen_pll_enable,
		.disable = msc313e_clkgen_pll_disable,
		.is_enabled = msc313e_clkgen_pll_is_enabled,
		.recalc_rate = msc313e_clkgen_pll_recalc_rate
};

static const struct regmap_config msc313_pll_regmap_config = {
                .name = "msc313-pll",
                .reg_bits = 16,
                .val_bits = 16,
                .reg_stride = 4,
};

static int msc313e_clkgen_pll_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct msc313e_clkgen_pll* clkgen_pll;
	struct clk_init_data *clk_init;
	struct clk* clk;
	struct resource *mem, *base;
	int numparents, numoutputs, numrates, pllindex;
	struct clk_onecell_data *clk_data;
	const char *parents[16];
	struct regmap* regmap;
	u32 upstream;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(msc313e_clkgen_pll_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, 16);

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

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base,
                        &msc313_pll_regmap_config);
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

	for (pllindex = 0; pllindex < numoutputs; pllindex++) {
		clkgen_pll = devm_kzalloc(&pdev->dev, sizeof(*clkgen_pll), GFP_KERNEL);
		if (!clkgen_pll)
			return -ENOMEM;

		clkgen_pll->regmap = regmap;
		clkgen_pll->mask = 1 << pllindex;
		of_property_read_u32_index(pdev->dev.of_node, "clock-rates",
				pllindex, &clkgen_pll->rate);

		clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
		if (!clk_init)
			return -ENOMEM;

		clkgen_pll->clk_hw.init = clk_init;
		of_property_read_string_index(pdev->dev.of_node,
				"clock-output-names", pllindex, &clk_init->name);
		clk_init->ops = &msc313e_clkgen_pll_ops;
		//clk_init->flags = CLK_IGNORE_UNUSED;

		of_property_read_u32_index(pdev->dev.of_node, "clock-upstreams",
				pllindex, &upstream);
		clk_init->num_parents = 1;
		clk_init->parent_names = parents + upstream;

		clk = clk_register(&pdev->dev, &clkgen_pll->clk_hw);
		if (IS_ERR(clk)) {
			printk("failed to register clk");
			return -ENOMEM;
		}
		clk_data->clks[pllindex] = clk;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);
}

static int msc313e_clkgen_pll_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313e_clkgen_pll_driver = {
	.driver = {
		.name = "msc313e-clkgen-pll",
		.of_match_table = msc313e_clkgen_pll_of_match,
	},
	.probe = msc313e_clkgen_pll_probe,
	.remove = msc313e_clkgen_pll_remove,
};
module_platform_driver(msc313e_clkgen_pll_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MSC313e clkgen pll driver");
