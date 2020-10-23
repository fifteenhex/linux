// SPDX-License-Identifier: GPL-2.0
/*
 * MStar MSC313 MPLL driver
 *
 * Copyright (C) 2020 Daniel Palmer <daniel@thingy.jp>
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of_address.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define REG_CONFIG1	0x8
#define REG_CONFIG2	0xc

static const struct regmap_config msc313_mpll_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static const struct reg_field config1_loop_div_first = REG_FIELD(REG_CONFIG1, 8, 9);
static const struct reg_field config1_input_div_first = REG_FIELD(REG_CONFIG1, 4, 5);
static const struct reg_field config2_output_div_first = REG_FIELD(REG_CONFIG2, 12, 13);
static const struct reg_field config2_loop_div_second = REG_FIELD(REG_CONFIG2, 0, 7);

static const unsigned int output_dividers[] = {
	2, 3, 4, 5, 6, 7, 10
};

#define NUMOUTPUTS (ARRAY_SIZE(output_dividers) + 1)

struct msc313_mpll {
	struct clk_hw clk_hw;
	struct regmap_field *input_div;
	struct regmap_field *loop_div_first;
	struct regmap_field *loop_div_second;
	struct regmap_field *output_div;
	struct clk_hw_onecell_data *clk_data;
};

#define to_mpll(_hw) container_of(_hw, struct msc313_mpll, clk_hw)
#define to_divider_hw(_mpll, _which) _mpll->clk_data->hws[_which + 1]

static unsigned long msc313_mpll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct msc313_mpll *mpll = to_mpll(hw);
	unsigned int input_div, output_div, loop_first, loop_second;
	unsigned long output_rate;

	regmap_field_read(mpll->input_div, &input_div);
	regmap_field_read(mpll->output_div, &output_div);
	regmap_field_read(mpll->loop_div_first, &loop_first);
	regmap_field_read(mpll->loop_div_second, &loop_second);

	output_rate = parent_rate / (1 << input_div);
	output_rate *= (1 << loop_first) * max(loop_second, 1U);
	output_rate /= max(output_div, 1U);

	return output_rate;
}

static const struct clk_ops msc313_mpll_ops = {
		.recalc_rate = msc313_mpll_recalc_rate,
};

static int msc313_mpll_probe(struct platform_device *pdev)
{
	void __iomem *base;
	struct msc313_mpll *mpll;
	struct clk_init_data clk_init;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	const char *parents[1], *outputnames[NUMOUTPUTS];
	const int numparents = ARRAY_SIZE(parents);
	int ret, i;

	if (of_clk_parent_fill(dev->of_node, parents, numparents) != numparents)
		return -EINVAL;

	if (of_property_read_string_array(pdev->dev.of_node, "clock-output-names",
			outputnames, NUMOUTPUTS) != NUMOUTPUTS)
		return -EINVAL;

	mpll = devm_kzalloc(dev, sizeof(*mpll), GFP_KERNEL);
	if (!mpll)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &msc313_mpll_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	mpll->input_div = devm_regmap_field_alloc(dev, regmap, config1_input_div_first);
	if (IS_ERR(mpll->input_div))
		return PTR_ERR(mpll->input_div);
	mpll->output_div = devm_regmap_field_alloc(dev, regmap, config2_output_div_first);
	if (IS_ERR(mpll->output_div))
		return PTR_ERR(mpll->output_div);
	mpll->loop_div_first = devm_regmap_field_alloc(dev, regmap, config1_loop_div_first);
	if (IS_ERR(mpll->loop_div_first))
		return PTR_ERR(mpll->loop_div_first);
	mpll->loop_div_second = devm_regmap_field_alloc(dev, regmap, config2_loop_div_second);
	if (IS_ERR(mpll->loop_div_second))
		return PTR_ERR(mpll->loop_div_second);

	mpll->clk_data = devm_kzalloc(dev, struct_size(mpll->clk_data, hws,
			ARRAY_SIZE(output_dividers)), GFP_KERNEL);
	if (!mpll->clk_data)
		return -ENOMEM;

	clk_init.name = outputnames[0];
	clk_init.ops = &msc313_mpll_ops;
	clk_init.num_parents = 1;
	clk_init.parent_names = parents;
	mpll->clk_hw.init = &clk_init;

	ret = devm_clk_hw_register(dev, &mpll->clk_hw);
	if (ret)
		return ret;

	mpll->clk_data->num = NUMOUTPUTS;
	mpll->clk_data->hws[0] = &mpll->clk_hw;

	for (i = 0; i < ARRAY_SIZE(output_dividers); i++) {
		to_divider_hw(mpll, i) = clk_hw_register_fixed_factor(dev,
				outputnames[i + 1], outputnames[0], 0, 1, output_dividers[i]);
		if (IS_ERR(to_divider_hw(mpll, i))) {
			ret = PTR_ERR(to_divider_hw(mpll, i));
			goto unregister_dividers;
		}
	}

	platform_set_drvdata(pdev, mpll);

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get,
			mpll->clk_data);

unregister_dividers:
	for (i--; i >= 0; i--)
		clk_hw_unregister_fixed_factor(to_divider_hw(mpll, i));
	return ret;
}

static int msc313_mpll_remove(struct platform_device *pdev)
{
	struct msc313_mpll *mpll = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(output_dividers); i++)
		clk_hw_unregister_fixed_factor(to_divider_hw(mpll, i));

	return 0;
}

static const struct of_device_id msc313_mpll_of_match[] = {
	{ .compatible = "mstar,msc313-mpll", },
	{}
};
MODULE_DEVICE_TABLE(of, msc313_mpll_of_match);

static struct platform_driver msc313_mpll_driver = {
	.driver = {
		.name = "mstar-msc313-mpll",
		.of_match_table = msc313_mpll_of_match,
	},
	.probe = msc313_mpll_probe,
	.remove = msc313_mpll_remove,
};
builtin_platform_driver(msc313_mpll_driver);
