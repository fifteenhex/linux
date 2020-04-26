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

/*
 * The clkgen block controls a bunch of clock gates and muxes
 * Each register contains gates, muxes and some sort of anti-glitch
 * control.
 *
 * This driver controls the gates and muxes packed into a single register.
 */

struct msc313e_clkgen_muxparent {
	void __iomem *base;
	u32 saved;
};

struct msc313e_clkgen_mux {
	struct msc313e_clkgen_muxparent *parent;
	struct clk_hw clk_hw;
	u8 shift;
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
	struct msc313e_clkgen_mux *clkgen_mux;
	struct clk_gate *gate;
	struct clk_mux *mux;
	struct clk* clk;
	int numparents, numshifts, nummuxes, muxindex, muxrangeoffset;
	struct clk_onecell_data *clk_data;
	const char *name;
	const char *parents[32];
	const char **muxparents;
	u32 gateshift, muxshift, muxwidth, muxclockoffset, muxnumclocks, outputflags;
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

	mux_parent->base = of_iomap(pdev->dev.of_node, 0);
	if (IS_ERR(mux_parent->base)){
		ret = PTR_ERR(mux_parent->base);
		goto out;
	}

	nummuxes = of_property_count_strings(pdev->dev.of_node,
			"clock-output-names");
	if (!nummuxes) {
		dev_info(&pdev->dev, "output names need to be specified");
		ret = -ENODEV;
		goto out;
	}

	numshifts = of_property_count_u32_elems(pdev->dev.of_node, "shifts");
	if(!numshifts){
		dev_info(&pdev->dev, "shifts need to be specified");
		ret =-ENODEV;
		goto out;
	}

	if(numshifts != nummuxes){
		dev_info(&pdev->dev, "number of shifts must match the number of outputs");
		ret = -EINVAL;
		goto out;
	}

	// check mux shifts, widths, ranges here

	clk_data = devm_kzalloc(&pdev->dev, sizeof(struct clk_onecell_data),
			GFP_KERNEL);
	if (!clk_data){
		ret = -ENOMEM;
		goto out;
	}

	clk_data->clk_num = nummuxes;
	clk_data->clks = devm_kcalloc(&pdev->dev, nummuxes,
			sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks){
		ret = -ENOMEM;
		goto out;
	}

	for (muxindex = 0; muxindex < nummuxes; muxindex++) {
		ret = of_property_read_string_index(pdev->dev.of_node, "clock-output-names",
				muxindex, &name);
		if(ret)
			goto out;

		ret = of_property_read_u32_index(pdev->dev.of_node, "shifts",
				muxindex, &gateshift);
		if(ret)
			goto out;

		clkgen_mux = devm_kzalloc(&pdev->dev, sizeof(*clkgen_mux),GFP_KERNEL);
		if (!clkgen_mux){
			ret = -ENOMEM;
			goto out;
		}
		clkgen_mux->parent = mux_parent;

		/* there is always a gate */
		gate = devm_kzalloc(&pdev->dev, sizeof(*gate), GFP_KERNEL);
		if(!gate){
			ret = -ENOMEM;
			goto out;
		}
		gate->reg = mux_parent->base;
		gate->bit_idx = gateshift;
		gate->flags = CLK_GATE_SET_TO_DISABLE;

		/* it's possible to not have a mux, there has to be some parents
		 * and a shift and width value for each output
		 */
		mux = NULL;

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

		mux = devm_kzalloc(&pdev->dev, sizeof(*mux), GFP_KERNEL);
		if(!mux){
			ret = -ENOMEM;
			goto out;
		}
		mux->reg = mux_parent->base;
		mux->shift = muxshift;
		mux->mask = ~((~0 >> muxwidth) << muxwidth);
		mux->flags = CLK_MUX_ROUND_CLOSEST;

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

		clk = clk_register_composite(&pdev->dev, name,
				muxparents, muxnumclocks,
				mux != NULL ? &mux->hw : NULL, mux != NULL ? &clk_mux_ops : NULL,
				NULL, NULL,
				&gate->hw, &clk_gate_ops,
				outputflags);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			goto out;
		}

		clk_data->clks[muxindex] = clk;
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
	parent->saved = readl_relaxed(parent->base);
	return 0;
}

static int __maybe_unused msc313e_clkgen_mux_resume(struct device *dev)
{
	struct msc313e_clkgen_muxparent *parent = platform_get_drvdata(to_platform_device(dev));
	u32 cur = readl_relaxed(parent->base);
	if(cur != parent->saved){
		dev_warn(dev, "mux was before %x but is now %x\n", parent->saved, cur);
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
