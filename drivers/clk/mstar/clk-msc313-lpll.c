// SPDX-License-Identifier: GPL-2.0
/*
 * MStar/SigmaStar display / MIPI "LPLL" (0x1f206700)
 *
 * Copyright (C) 2019 Daniel Palmer <daniel@thingy.jp>
 *
 * The LCD/MIPI PLL that generates the MIPI DSI functional clock and, through
 * the sc_pixel mux, the display pixel clock. The DSI and D-PHY register banks
 * are unclocked while it is off, and a RIU access to an unclocked block hangs
 * the bus, so the DSI host must enable this clock before it touches its
 * registers, and it must lock at the panel's rate or the DSI streams video
 * the panel cannot follow.
 *
 * The rate of this clock is the DSI data-lane bit rate (pixel clock * bpp /
 * lanes). It is programmed the way the vendor panel HAL does
 * (__HalPnlIfSetLpllConfig / HalPnlSetLpllSet):
 *
 *   band = VCO band for the lane rate (selects the divider chain registers)
 *   set  = (432 MHz * 2^19 * loop_gain / loop_div) / (lane rate in MHz)
 *
 * and, like the vendor, the PLL is first locked at the LP (command mode) rate
 * derived from a fixed 858x525@60 timing before the real video rate is
 * written, which lets the DSI settle before it streams pixels.
 */
#include <linux/platform_device.h>
#include <soc/mstar/regsave.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/io.h>

#define LPLL_BAND0	0x00	/* 0x2201 */
#define LPLL_BAND1	0x04	/* 0x0420 (constant) */
#define LPLL_BAND2	0x08	/* divider-chain select */
#define LPLL_BAND3	0x0c	/* post-divider */
#define LPLL_EN		0x10	/* 0x0900 = enable */
#define LPLL_SET_LO	0x20	/* loop divider [15:0] */
#define LPLL_SET_HI	0x24	/* loop divider [23:16] */
#define LPLL_LOAD0	0x28	/* 0x0001 */
#define LPLL_LOAD1	0x2c	/* 0x0000 */
#define LPLL_RESET0	0x38
#define LPLL_RESET1	0x3c

#define LPLL_EN_VAL	0x0900

/*
 * Per-band constants from the vendor LPLLSettingTBL / loop-gain tables for
 * the DSI link. Only these four bands (>= 100 Mbps) are reachable for DSI.
 *   band 0: 800..15000 Mbps  band 1: 400..800 Mbps
 *   band 2: 200..400 Mbps    band 3: 100..200 Mbps
 */
#define LPLL_NBANDS	4
static const u16 lpll_band0[LPLL_NBANDS] = { 0x2201, 0x2201, 0x2201, 0x2201 };
static const u16 lpll_band2[LPLL_NBANDS] = { 0x0041, 0x0042, 0x0043, 0x0083 };
static const u16 lpll_band3[LPLL_NBANDS] = { 0x0000, 0x0001, 0x0002, 0x0003 };
static const u16 lpll_gain[LPLL_NBANDS]  = { 16, 8, 4, 2 };

#define LPLL_MIN_RATE	100000000UL
#define LPLL_MAX_RATE	15000000000UL
#define LPLL_DIVIDEND	(432ULL * 524288ULL)	/* 432 MHz * 2^19 */

/* The fixed LP/command-mode timing the vendor pre-locks at. */
#define LPLL_LP_RATE	(858UL * 525UL * 60UL * 24UL / 4UL)

struct msc313_lpll {
	struct clk_hw clk_hw;
	void __iomem *base;
	struct mstar_regsave save;
	unsigned long rate;
	struct clk_hw_onecell_data *clk_data;
};

#define to_lpll(_hw) container_of(_hw, struct msc313_lpll, clk_hw)

static int lpll_band_idx(unsigned long rate)
{
	if (rate < LPLL_MIN_RATE)
		return -1;
	if (rate < 200000000UL)
		return 3;
	if (rate < 400000000UL)
		return 2;
	if (rate < 800000000UL)
		return 1;
	return 0;
}

static u32 lpll_set_val(int band, unsigned long rate)
{
	u32 divisor = rate / 1000000;
	u64 dividend = LPLL_DIVIDEND * lpll_gain[band];

	return divisor ? (u32)div_u64(dividend, divisor) : 0;
}

static void lpll_program(struct msc313_lpll *lpll, int band, u32 set)
{
	writew(lpll_band0[band], lpll->base + LPLL_BAND0);
	writew(0x0420, lpll->base + LPLL_BAND1);
	writew(lpll_band2[band], lpll->base + LPLL_BAND2);
	writew(lpll_band3[band], lpll->base + LPLL_BAND3);
	writew(0x0001, lpll->base + LPLL_LOAD0);
	writew(0x0000, lpll->base + LPLL_LOAD1);
	writew(set & 0xffff, lpll->base + LPLL_SET_LO);
	writew((set >> 16) & 0xff, lpll->base + LPLL_SET_HI);
}

static int msc313_lpll_program(struct msc313_lpll *lpll)
{
	int band = lpll_band_idx(lpll->rate);
	int lp_band = lpll_band_idx(LPLL_LP_RATE);

	if (band < 0)
		return -EINVAL;
	if (lp_band < 0)
		lp_band = band;

	writew(0x0000, lpll->base + LPLL_RESET0);
	writew(0x0000, lpll->base + LPLL_RESET1);
	writew(LPLL_EN_VAL, lpll->base + LPLL_EN);
	lpll_program(lpll, lp_band, lpll_set_val(lp_band, LPLL_LP_RATE));
	lpll_program(lpll, band, lpll_set_val(band, lpll->rate));

	return 0;
}

static int msc313_lpll_enable(struct clk_hw *hw)
{
	struct msc313_lpll *lpll = to_lpll(hw);

	/*
	 * Enabled through a consumer that has not set a lane rate yet (the
	 * DSI host enables its APB clock before it sets the rate): lock at
	 * the vendor's command-mode rate; set_rate() reprograms it later.
	 */
	if (!lpll->rate)
		lpll->rate = LPLL_LP_RATE;

	return msc313_lpll_program(lpll);
}

static void msc313_lpll_disable(struct clk_hw *hw)
{
	/*
	 * Deliberately left running: the DSI/D-PHY banks hang the bus when
	 * accessed with the PLL off, and nothing else gates it. Keep it on.
	 */
}

static int msc313_lpll_is_enabled(struct clk_hw *hw)
{
	struct msc313_lpll *lpll = to_lpll(hw);

	return readw(lpll->base + LPLL_EN) == LPLL_EN_VAL;
}

static unsigned long msc313_lpll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct msc313_lpll *lpll = to_lpll(hw);

	return lpll->rate;
}

static int msc313_lpll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	req->rate = clamp(req->rate, LPLL_MIN_RATE, LPLL_MAX_RATE);
	return 0;
}

static int msc313_lpll_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct msc313_lpll *lpll = to_lpll(hw);

	if (lpll_band_idx(rate) < 0)
		return -EINVAL;

	lpll->rate = rate;
	if (msc313_lpll_is_enabled(hw))
		return msc313_lpll_program(lpll);

	return 0;
}

static const struct clk_ops msc313_lpll_ops = {
	.enable = msc313_lpll_enable,
	.disable = msc313_lpll_disable,
	.is_enabled = msc313_lpll_is_enabled,
	.recalc_rate = msc313_lpll_recalc_rate,
	.determine_rate = msc313_lpll_determine_rate,
	.set_rate = msc313_lpll_set_rate,
};

static const struct clk_parent_data lpll_parent = {
	.index	= 0,
};

static int msc313_lpll_probe(struct platform_device *pdev)
{
	struct clk_init_data clk_init = { };
	struct device *dev = &pdev->dev;
	struct msc313_lpll *lpll;
	int ret;

	lpll = devm_kzalloc(dev, sizeof(*lpll), GFP_KERNEL);
	if (!lpll)
		return -ENOMEM;

	lpll->clk_data = devm_kzalloc(dev, struct_size(lpll->clk_data, hws, 1),
			GFP_KERNEL);
	if (!lpll->clk_data)
		return -ENOMEM;
	lpll->clk_data->num = 1;

	lpll->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lpll->base))
		return PTR_ERR(lpll->base);
	ret = mstar_regsave_init(&pdev->dev, &lpll->save, lpll->base, 0x40);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, lpll);

	clk_init.name = dev_name(dev);
	clk_init.ops = &msc313_lpll_ops;
	clk_init.parent_data = &lpll_parent;
	clk_init.num_parents = 1;
	lpll->clk_hw.init = &clk_init;

	ret = devm_clk_hw_register(dev, &lpll->clk_hw);
	if (ret)
		return ret;

	lpll->clk_data->hws[0] = &lpll->clk_hw;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get,
			lpll->clk_data);
}

static const struct of_device_id msc313_lpll_of_match[] = {
	{
		.compatible = "mstar,msc313-lpll",
	},
	{}
};


/* The sleep reset wipes the registers, put them back the way they were */
static int msc313_lpll_sleep_save(struct device *dev)
{
	struct msc313_lpll *priv = dev_get_drvdata(dev);

	mstar_regsave_save(&priv->save);
	return 0;
}

static int msc313_lpll_sleep_restore(struct device *dev)
{
	struct msc313_lpll *priv = dev_get_drvdata(dev);

	mstar_regsave_restore(&priv->save);
	return 0;
}

static const struct dev_pm_ops msc313_lpll_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(msc313_lpll_sleep_save, msc313_lpll_sleep_restore)
};

static struct platform_driver msc313_lpll_driver = {
	.driver = {
		.name = "mstar-lpll",
		.of_match_table = msc313_lpll_of_match,
		.pm = pm_sleep_ptr(&msc313_lpll_pm_ops),
	},
	.probe = msc313_lpll_probe,
};
builtin_platform_driver(msc313_lpll_driver);
