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

#define DT_MSTAR_DEGLITCHES "mstar,deglitches"

/*
 * The clkgen block controls a bunch of clock gates and muxes
 * Each register contains gates, muxes and some sort of anti-glitch
 * control.
 *
 * This driver controls the gates and muxes packed into a single register.
 */

struct msc313e_clkgen_mux;

struct msc313e_clkgen_muxparent {
	void __iomem *base;
	struct clk* clk;
	struct msc313e_clkgen_mux *muxes;
	unsigned nummuxes;
	u32 saved;
};

struct msc313e_clkgen_mux {
	struct msc313e_clkgen_muxparent *parent;
	struct clk_hw clk_hw;
	u8 shift;
	u16 deglitch;
	struct clk_gate gate;
	struct clk_mux mux;
};

#define to_clkgen_mux(_hw) container_of(_hw, struct msc313e_clkgen_mux, clk_hw)

static const struct of_device_id msc313e_clkgen_mux_of_match[] = {
	{
		.compatible = "mstar,msc313e-clkgen-mux",
	},
	{}
};
MODULE_DEVICE_TABLE(of, msc313e_clkgen_mux_of_match);

static int msc313e_clkgen_mux_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct msc313e_clkgen_muxparent *mux_parent;
	int muxindex, muxrangeoffset;
	struct clk_onecell_data *clk_data;
	const char *name;
	const char *parents[32];
	const char **muxparents;
	int numparents, numshifts, numdeglitches;
	u32 gateshift, muxshift, muxwidth, muxclockoffset, muxnumclocks, outputflags, deglitch;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(msc313e_clkgen_mux_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents,
			ARRAY_SIZE(parents));

	if(numparents < 0){
		dev_info(&pdev->dev, "failed to get clock parents\n");
		ret = numparents;
		goto out;
	}
	else if(numparents == 0){
		dev_dbg(&pdev->dev, "no parent clocks, gating only\n");
	}

	mux_parent = devm_kzalloc(&pdev->dev, sizeof(*mux_parent),GFP_KERNEL);
	if(IS_ERR(mux_parent)){
		ret = PTR_ERR(mux_parent);
		goto out;
	}

	mux_parent->nummuxes = of_property_count_strings(pdev->dev.of_node,
			"clock-output-names");

	mux_parent->muxes = devm_kcalloc(&pdev->dev, mux_parent->nummuxes,
			sizeof(*mux_parent->muxes),GFP_KERNEL);
	if (!mux_parent->muxes){
		ret = -ENOMEM;
		goto out;
	}

	if (!mux_parent->nummuxes) {
		dev_info(&pdev->dev, "output names need to be specified\n");
		ret = -ENODEV;
		goto out;
	}

	mux_parent->base = of_iomap(pdev->dev.of_node, 0);
	if (IS_ERR(mux_parent->base)){
		ret = PTR_ERR(mux_parent->base);
		goto out;
	}

	numshifts = of_property_count_u32_elems(pdev->dev.of_node, "shifts");
	if(numshifts < 0){
		dev_info(&pdev->dev, "shifts need to be specified");
		ret =-ENODEV;
		goto out;
	}

	if(numshifts != mux_parent->nummuxes){
		dev_info(&pdev->dev, "number of shifts must match the number of outputs\n");
		ret = -EINVAL;
		goto out;
	}

	numdeglitches = of_property_count_u32_elems(pdev->dev.of_node, DT_MSTAR_DEGLITCHES);
	if(numdeglitches > 0 && numdeglitches != mux_parent->nummuxes){
		dev_info(&pdev->dev, "number of deglitches must match the number of outputs\n");
		ret = -EINVAL;
		goto out;
	}

	clk_data = devm_kzalloc(&pdev->dev, sizeof(struct clk_onecell_data),
			GFP_KERNEL);
	if (!clk_data){
		ret = -ENOMEM;
		goto out;
	}

	clk_data->clk_num = mux_parent->nummuxes;
	clk_data->clks = devm_kcalloc(&pdev->dev, mux_parent->nummuxes,
			sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks){
		ret = -ENOMEM;
		goto out;
	}

	for (muxindex = 0; muxindex < mux_parent->nummuxes; muxindex++) {
		if(numdeglitches > 0){
			ret = of_property_read_u32_index(pdev->dev.of_node, DT_MSTAR_DEGLITCHES,
					muxindex, &deglitch);
			if(ret)
				goto out;
			mux_parent->muxes[muxindex].deglitch = BIT(deglitch);
		}

		ret = of_property_read_string_index(pdev->dev.of_node, "clock-output-names",
				muxindex, &name);
		if(ret)
			goto out;

		ret = of_property_read_u32_index(pdev->dev.of_node, "shifts",
				muxindex, &gateshift);
		if(ret)
			goto out;

		mux_parent->muxes[muxindex].parent = mux_parent;
		mux_parent->muxes[muxindex].gate.reg = mux_parent->base;
		mux_parent->muxes[muxindex].gate.bit_idx = gateshift;
		mux_parent->muxes[muxindex].gate.flags = CLK_GATE_SET_TO_DISABLE;

		if(numparents == 0)
			goto outputflags;

		ret = of_property_read_u32_index(pdev->dev.of_node, "mux-shifts",
				muxindex, &muxshift);
		if(ret)
			goto out;
		ret = of_property_read_u32_index(pdev->dev.of_node, "mux-widths",
				muxindex, &muxwidth);
		if(ret)
			goto out;

		mux_parent->muxes[muxindex].mux.reg = mux_parent->base;
		mux_parent->muxes[muxindex].mux.shift = muxshift;
		mux_parent->muxes[muxindex].mux.mask = ~((~0 >> muxwidth) << muxwidth);
		mux_parent->muxes[muxindex].mux.flags = CLK_MUX_ROUND_CLOSEST;

		muxrangeoffset = muxindex * 2;
		ret = of_property_read_u32_index(pdev->dev.of_node, "mux-ranges",
				muxrangeoffset, &muxclockoffset);
		if(ret)
			goto allparents;

		ret = of_property_read_u32_index(pdev->dev.of_node, "mux-ranges",
				muxrangeoffset + 1, &muxnumclocks);
		if(ret)
			goto allparents;

		muxparents = parents + muxclockoffset;
		dev_dbg(&pdev->dev, "using clocks %d -> %d for mux",
			(int)(muxclockoffset), (int)(muxclockoffset + muxnumclocks));

		goto outputflags;

allparents:
		muxparents = parents;
		muxnumclocks = numparents;
		dev_dbg(&pdev->dev, "clock range not specified, mux will use all clocks");

outputflags:
		outputflags = 0;
		of_property_read_u32_index(pdev->dev.of_node, "output-flags",
				muxindex, &outputflags);
		if(!ret){
			dev_dbg(&pdev->dev, "applying flags %x to output %d",
					outputflags, muxindex);
		}

		mux_parent[muxindex].clk = clk_register_composite(&pdev->dev, name,
				muxparents, muxnumclocks,
				numparents ? &mux_parent->muxes[muxindex].mux.hw : NULL,
				numparents ? &clk_mux_ops : NULL,
				NULL,
				NULL,
				&mux_parent->muxes[muxindex].gate.hw,
				&clk_gate_ops,
				outputflags);
		if (IS_ERR(mux_parent[muxindex].clk)) {
			ret = PTR_ERR(mux_parent[muxindex].clk);
			goto out;
		}
		clk_data->clks[muxindex] = mux_parent[muxindex].clk;
	}

	platform_set_drvdata(pdev, mux_parent);

	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);

out:
	return ret;
}

static int msc313e_clkgen_mux_remove(struct platform_device *pdev)
{
	return 0;
}

static int __maybe_unused msc313e_clkgen_mux_suspend(struct device *dev)
{
	struct msc313e_clkgen_muxparent *parent = platform_get_drvdata(to_platform_device(dev));
	int i;
	u16 deglitch = 0;
	parent->saved = readl_relaxed(parent->base);
	for(i = 0; i < parent->nummuxes; i++)
		deglitch |= parent->muxes[i].deglitch;
	if(deglitch != 0)
		writel_relaxed(parent->saved & ~deglitch, parent->base);
	return 0;
}

static int __maybe_unused msc313e_clkgen_mux_resume(struct device *dev)
{
	struct msc313e_clkgen_muxparent *parent = platform_get_drvdata(to_platform_device(dev));
	u32 cur = readl_relaxed(parent->base);
	if(cur != parent->saved){
		dev_warn(dev, "mux was before %x but is now %x, restoring\n", parent->saved, cur);
		writel_relaxed(parent->saved, parent->base);
	}
	return 0;
}

static SIMPLE_DEV_PM_OPS(msc313e_clkgen_mux_pm_ops, msc313e_clkgen_mux_suspend,
			 msc313e_clkgen_mux_resume);

static struct platform_driver msc313e_clkgen_mux_driver = {
	.driver = {
		.name = "msc313e-clkgen-mux",
		.of_match_table = msc313e_clkgen_mux_of_match,
		.pm = &msc313e_clkgen_mux_pm_ops,
	},
	.probe = msc313e_clkgen_mux_probe,
	.remove = msc313e_clkgen_mux_remove,
};
module_platform_driver(msc313e_clkgen_mux_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MSC313e clkgen mux driver");
