struct mstar_pll_output;

struct mstar_pll {
	void __iomem *base;
	struct clk_onecell_data clk_data;
	struct mstar_pll_output *outputs;
	unsigned numoutputs;
};

struct mstar_pll_output {
	struct mstar_pll *pll;
	u32 rate;
	struct clk_hw clk_hw;
};

#define to_pll_output(_hw) container_of(_hw, struct mstar_pll_output, clk_hw)

int mstar_pll_common_probe(struct platform_device *pdev, struct mstar_pll **pll,
		const struct clk_ops *clk_ops);
