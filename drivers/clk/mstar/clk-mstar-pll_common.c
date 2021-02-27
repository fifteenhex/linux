#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>

#include "clk-mstar-pll_common.h"

int mstar_pll_common_probe(struct platform_device *pdev, struct mstar_pll **pll,
		const struct clk_ops *clk_ops)
{
	struct mstar_pll_output* output;
	struct clk_init_data clk_init = { };
	struct clk* clk;
	int numparents, numoutputs, numrates, pllindex;
	struct clk_onecell_data *clk_data;
	const char *parents[1];

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

	*pll = devm_kzalloc(&pdev->dev, sizeof(**pll), GFP_KERNEL);
	(*pll)->outputs = devm_kzalloc(&pdev->dev, sizeof((*pll)->outputs) * numoutputs, GFP_KERNEL);
	if (!(*pll)->outputs)
		return -ENOMEM;

	(*pll)->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR((*pll)->base))
		return PTR_ERR((*pll)->base);

	clk_data = &(*pll)->clk_data;
	clk_data->clk_num = numoutputs;
	clk_data->clks = devm_kcalloc(&pdev->dev, numoutputs, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		return -ENOMEM;

	for (pllindex = 0; pllindex < numoutputs; pllindex++) {
		output = &(*pll)->outputs[pllindex];
		output->pll = *pll;

		of_property_read_u32_index(pdev->dev.of_node, "clock-rates",
				pllindex, &output->rate);

		output->clk_hw.init = &clk_init;
		of_property_read_string_index(pdev->dev.of_node,
				"clock-output-names", pllindex, &clk_init.name);
		clk_init.ops = clk_ops;
		clk_init.num_parents = 1;
		clk_init.parent_names = parents;

		clk = clk_register(&pdev->dev, &output->clk_hw);
		if (IS_ERR(clk)) {
			printk("failed to register clk");
			return -ENOMEM;
		}
		clk_data->clks[pllindex] = clk;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get, clk_data);
}
