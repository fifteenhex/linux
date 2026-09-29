// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Daniel Palmer
 */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mstar-msc313-pm-muxes.h>

#include "clk-msc313-mux.h"

#if 0


			clkgen_mux_spi_mcu_pm: clkgen_mux@1c80 {
				compatible = "mstar,msc313e-clkgen-mux";
				reg = <0x1c80 0x4>;
				#clock-cells = <1>;
				clock-output-names = "mcu_pm", "spi_pm";
				shifts = <0>, <8>;
				mstar,deglitches = <7>, <14>;
				mux-shifts = <2>, <10>;
				mux-widths = <4>, <4>;
				mux-ranges = <0 2>, <2 17>;
				clocks = /* mcu_pm */
					 <&clkgen_pll MSTAR_MPLL_GATE_MPLL_216>,
					 /* deglitch */
					 <&xtal12>,
					 /* spi_pm */
					 <&rtc_xtal>,
					 <&clk_mpll_216_div8>,
					 <&clk_mpll_144_div4>,
					 <&clk_mpll_86_div2>,
					 <&clk_mpll_216_div4>,
					 <&clk_mpll_144_div2>,
					 <&clkgen_pll MSTAR_MPLL_GATE_MPLL_86>,
					 <&clk_mpll_216_div2>,
					 <&xtal_div2_div8>,
					 <&xtal_div2_div12>,
					 <&rtc32k_div4>,
					 <&xtal_div2_div16>,
					 <&xtal_div2_div2>,
					 <&xtal_div2_div4>,
					 <&xtal12>,
					 <&xtal>,
					 /* deglitch */
					 <&xtal12>;
				assigned-clocks = <&clkgen_mux_spi_mcu_pm 1>;
				assigned-clock-parents = <&clk_mpll_216_div4>;
				assigned-clock-rates = <0>;
			};
#endif

static const struct clk_parent_data mcu_pm_parents[] = {
	{ .fw_name = "mpll_div_4" },
};

/*
 * This is the clock for the SPI flash controller and the QSPI read
 * path. The order is from the vendor's clock tree (CLK_spi_pm), and
 * matches the values their flash driver uses: 4 = 54MHz, 6 = 86MHz,
 * 7 = 108MHz. The deglitch bit (14) switches between the 12MHz xtal
 * and the selected parent, the boot ROM leaves it on the xtal.
 *
 * The names are in terms of the MPLL driver's outputs, where the
 * MPLL is 864MHz so mpll_div_4 is the 216MHz the vendor tree calls
 * mpll_216m. On SSD202D only the parents derived from that 216MHz
 * output (27, 54 and 108MHz) produce a working clock; selecting any
 * of the ones derived from the 144MHz or 86.4MHz outputs stops the
 * flash controller dead. The DT only provides the ones that work,
 * the rest are here so the indexes line up.
 */
static const struct clk_parent_data spi_pm_parents[] = {
	{ .fw_name = "rtc_xtal" },
	{ .fw_name = "mpll_div_4_div8" },	/* 27MHz */
	{ .fw_name = "mpll_div_6_div4" },	/* 36MHz */
	{ .fw_name = "mpll_div_10_div2" },	/* 43.2MHz */
	{ .fw_name = "mpll_div_4_div4" },	/* 54MHz */
	{ .fw_name = "mpll_div_6_div2" },	/* 72MHz */
	{ .fw_name = "mpll_div_10" },		/* 86.4MHz */
	{ .fw_name = "mpll_div_4_div2" },	/* 108MHz */
	{ .fw_name = "xtal_div2_div8" },
	{ .fw_name = "xtal_div2_div12" },
	{ .fw_name = "rtc_xtal_div4" },
	{ .fw_name = "xtal_div2_div16" },
	{ .fw_name = "xtal_div2_div2" },
	{ .fw_name = "xtal_div2_div4" },
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "xtal" },
};

#define MCU_PM MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_MCU_PM, "mcu_pm", mcu_pm_parents, 0x80, 0, 2, 4, 7)
#define SPI_PM MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_SPI_PM, "spi_pm", spi_pm_parents, 0x80, 8, 10, 4, 14)

static const struct clk_parent_data ir_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
	{ .fw_name = "xtal_div2_div8" },
	{ .fw_name = "xtal_div2_div12" },
	{ .fw_name = "rtc_xtal_div4" },
	{ .fw_name = "xtal_div2_div16" },
	{ .fw_name = "xtal_div2_div2" },
	{ .fw_name = "xtal_div2_div4" },
};

#define IR MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_IR, "ir", ir_parents, 0x84, 5, 7, 3, -1)

static const struct clk_parent_data rtc_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
};

static const struct clk_parent_data sar_pm_sleep_parents[] = {
	{ .fw_name = "xtal_div2" },
	{ .fw_name = "rtc_xtal" },
	{ .fw_name = "xtal_div2_div8" },
	{ .fw_name = "xtal_div2_div12" },
	{ .fw_name = "rtc_xtal_div4" },
	{ .fw_name = "xtal_div2_div16" },
	{ .fw_name = "xtal_div2_div2" },
	{ .fw_name = "xtal_div2_div4" },
};

#define RTC		MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_RTC, "rtc", rtc_parents, 0x88, 0, 2, 2, -1)
#define SAR		MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_SAR, "sar", sar_pm_sleep_parents, 0x88, 5, 7, 3, -1)
#define PM_SLEEP	MSC313_MUX_CLK_PARENT_DATA(MSC313_PM_MUXES_PM_SLEEP, "pm_sleep", sar_pm_sleep_parents, 0x88, 10, 12, 3, -1)

static const struct msc313_mux_data msc313_muxes[] = {
	MCU_PM,
	SPI_PM,
	IR,
	RTC,
	SAR,
	PM_SLEEP,
};

static const struct msc313_muxes_data msc313_data = MSC313_MUXES_DATA(msc313_muxes);

static const struct msc313_mux_data ssd20xd_muxes[] = {
	MCU_PM,
	SPI_PM,
	IR,
	RTC,
	SAR,
	PM_SLEEP,
};

static const struct msc313_muxes_data ssd20xd_data = MSC313_MUXES_DATA(ssd20xd_muxes);

static const struct of_device_id msc313_pm_muxes_ids[] = {
	{
		.compatible = "mstar,msc313-pm-muxes",
		.data = &msc313_data,
	},
	{
		.compatible = "sstar,ssd20xd-pm-muxes",
		.data = &ssd20xd_data,
	},
	{}
};

static int msc313_pm_muxes_probe(struct platform_device *pdev)
{
	const struct msc313_muxes_data *muxes_data;
	struct device *dev = &pdev->dev;
	struct msc313_muxes *muxes;
	struct regmap *regmap;

	muxes_data = of_device_get_match_data(dev);
	if (!muxes_data)
		return -EINVAL;

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);


	muxes = msc313_mux_register_muxes(dev, regmap, muxes_data, NULL, NULL);

	return devm_of_clk_add_hw_provider(dev, msc313_mux_xlate, muxes);
}

static struct platform_driver msc313_pm_muxes_driver = {
	.driver = {
		.pm = &msc313_mux_pm_ops,
		.name = "msc313-pm-muxes",
		.of_match_table = msc313_pm_muxes_ids,
	},
	.probe = msc313_pm_muxes_probe,
};
builtin_platform_driver(msc313_pm_muxes_driver);
