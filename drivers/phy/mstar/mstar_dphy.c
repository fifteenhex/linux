// SPDX-License-Identifier: GPL-2.0
/*
 * MStar/SigmaStar MIPI DSI D-PHY (0x1f2a5000)
 *
 * Copyright (C) 2021 Daniel Palmer <daniel@0x0f.com>
 *
 * The analog D-PHY that serialises the DSI host's output onto the physical
 * MIPI lanes. Bringing it up is the vendor's HalPnlInitMipiDsiDphy +
 * HalPnlSetMipiDsiChSel: release the power-downs and enable the fixed
 * clock/bias, then program the channel -> physical lane map. The map is
 * board wiring (the Miyoo Mini routes the clock and data channels through a
 * swapped lane order), so it comes from the device tree and defaults to the
 * identity. The HS/LP line-termination trims live in the eFuse and are
 * applied from nvmem cells when the board provides them.
 *
 * 16-bit registers on the usual 4-byte RIU stride. The bank is only
 * accessible while the mipi_tx_dsi APB clock is on, so the DSI host enables
 * its clocks before it powers this PHY on.
 *
 *  0x0
 *    6    |   0
 *  pd_ldo | sw_rst
 *
 *  0x4
 *               1               |          0
 *  power down whole dphy analog | power down hs mode
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "mstar-mipi_dphy"

#define DPHY_PD_LDO	0x00	/* LDO power-down release */
#define DPHY_PD		0x04	/* PHY + input-buffer power-down */
#define DPHY_CKEN	0x0c	/* fixed 1x clock enable / clock lane */
#define DPHY_DA0	0x10	/* data lane 0 P/N */
#define DPHY_CH0_SEL	0x18
#define DPHY_DA1	0x20
#define DPHY_CH1_SEL	0x28
#define DPHY_DA2	0x30
#define DPHY_CH2_SEL	0x38
#define DPHY_DUMMY0	0x50	/* also holds the LP termination trim, [12:11] */
#define DPHY_LPPATH	0x70	/* switch LP data path */
#define DPHY_HSBIST	0x74	/* HS BIST enable */
#define DPHY_DA3	0x80
#define DPHY_CH3_SEL	0x88
#define DPHY_DA4	0x8c
#define DPHY_CH4_SEL	0x94
#define DPHY_CLKSEL	0xd4	/* reg_clk_dsi_phy */
#define DPHY_HS_RTERM0	0x114	/* five lanes, 4 bytes apart, field [1:0] */

#define DPHY_NCHANNELS	5
#define DPHY_CH_EN	BIT(0)
#define DPHY_CH_SEL(lane)	(((lane) << 12) | ((lane) << 2) | DPHY_CH_EN)
#define DPHY_LP_RTERM_MASK	0x1800
#define DPHY_LP_RTERM_SHIFT	11

static const u16 dphy_ch_reg[DPHY_NCHANNELS] = {
	DPHY_CH0_SEL, DPHY_CH1_SEL, DPHY_CH2_SEL, DPHY_CH3_SEL, DPHY_CH4_SEL,
};

struct mstar_dphy {
	struct device *dev;
	struct phy *phy;
	void __iomem *base;
	u8 lane_map[DPHY_NCHANNELS];
	bool have_trims;
	u8 hs_rterm;
	u8 lp_rterm;
};

static void dphy_write(struct mstar_dphy *dphy, unsigned int off, u16 val)
{
	writew(val, dphy->base + off);
}

static void dphy_update(struct mstar_dphy *dphy, unsigned int off, u16 mask, u16 val)
{
	u16 tmp = readw(dphy->base + off);

	writew((tmp & ~mask) | (val & mask), dphy->base + off);
}

static int mstar_dphy_init(struct phy *phy)
{
	return 0;
}

static int mstar_dphy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	return 0;
}

static int mstar_dphy_power_on(struct phy *phy)
{
	struct mstar_dphy *dphy = phy_get_drvdata(phy);
	int i;

	/* enable all five channel lanes before the core comes up */
	for (i = 0; i < DPHY_NCHANNELS; i++)
		dphy_write(dphy, dphy_ch_reg[i], DPHY_CH_EN);

	dphy_write(dphy, DPHY_PD_LDO, 0x0001);	/* release LDO power-down */
	usleep_range(1000, 2000);
	dphy_write(dphy, DPHY_PD, 0x0000);	/* power up PHY + input buffer */
	dphy_write(dphy, DPHY_CKEN, 0x0f93);	/* fixed 1x clock on */
	dphy_write(dphy, DPHY_DUMMY0, 0x0080);
	dphy_write(dphy, DPHY_LPPATH, 0xc000);	/* switch LP data path */
	dphy_write(dphy, DPHY_HSBIST, 0x0080);
	dphy_write(dphy, DPHY_DA0, 0x0000);
	dphy_write(dphy, DPHY_DA1, 0x0000);
	dphy_write(dphy, DPHY_DA2, 0x0000);
	dphy_write(dphy, DPHY_DA3, 0x0000);
	dphy_write(dphy, DPHY_DA4, 0x0000);
	dphy_write(dphy, DPHY_CLKSEL, 0x0000);

	/* re-assert the lane enables and the fixed clock, then the lane map */
	for (i = 0; i < DPHY_NCHANNELS; i++)
		dphy_write(dphy, dphy_ch_reg[i], DPHY_CH_EN);
	dphy_write(dphy, DPHY_CKEN, 0x0f93);
	for (i = 0; i < DPHY_NCHANNELS; i++)
		dphy_write(dphy, dphy_ch_reg[i], DPHY_CH_SEL(dphy->lane_map[i]));

	/* termination trims, after the init that writes DUMMY0 */
	if (dphy->have_trims) {
		for (i = 0; i < DPHY_NCHANNELS; i++)
			dphy_update(dphy, DPHY_HS_RTERM0 + i * 4, 0x0003, dphy->hs_rterm);
		dphy_update(dphy, DPHY_DUMMY0, DPHY_LP_RTERM_MASK,
			    dphy->lp_rterm << DPHY_LP_RTERM_SHIFT);
	}

	dev_dbg(dphy->dev, "powered on, lane map %u %u %u %u %u, trims %s\n",
		dphy->lane_map[0], dphy->lane_map[1], dphy->lane_map[2],
		dphy->lane_map[3], dphy->lane_map[4],
		dphy->have_trims ? "applied" : "none");

	return 0;
}

static int mstar_dphy_power_off(struct phy *phy)
{
	struct mstar_dphy *dphy = phy_get_drvdata(phy);

	dphy_write(dphy, DPHY_PD, 0x0003);
	dphy_write(dphy, DPHY_PD_LDO, 0x0000);

	return 0;
}

static int mstar_dphy_exit(struct phy *phy)
{
	return 0;
}

static const struct phy_ops mstar_dphy_ops = {
	.configure	= mstar_dphy_configure,
	.power_on	= mstar_dphy_power_on,
	.power_off	= mstar_dphy_power_off,
	.init		= mstar_dphy_init,
	.exit		= mstar_dphy_exit,
};

/*
 * Word 3 of the eFuse carries HS_RTERM in [15:14], word 0xe carries a valid
 * bit in [15] and LP_RTERM in [14:13]; the cells hand us those fields
 * already shifted down: "hs-rterm" is 2 bits, "lp-rterm" is 3 bits with the
 * valid bit on top.
 *
 * Read with the "variable" accessor: these are 2 and 3 bit fields, so the cells
 * are smaller than a u32 and nvmem_cell_read_u32() rejects them outright with
 * -EINVAL, while this one trims a bit field cell to its real length.
 *
 * The eFuse provider lives in drivers/nvmem, which is linked long after
 * drivers/phy, so the first probe always runs before it has registered and the
 * cell read comes back -EPROBE_DEFER. Hand that to the caller instead of
 * swallowing it, or the lanes silently run untrimmed forever. A board whose DT
 * does not name the cells gets -ENOENT and simply has no trims.
 */
static int mstar_dphy_read_trims(struct mstar_dphy *dphy)
{
	u32 hs, lp;
	int ret;

	ret = nvmem_cell_read_variable_le_u32(dphy->dev, "hs-rterm", &hs);
	if (!ret)
		ret = nvmem_cell_read_variable_le_u32(dphy->dev, "lp-rterm", &lp);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_dbg(dphy->dev, "no MIPI eFuse trim cells (%d)\n", ret);
		return 0;
	}

	if (!(lp & BIT(2))) {
		dev_dbg(dphy->dev, "no MIPI eFuse trim (valid bit clear)\n");
		return 0;
	}

	dphy->hs_rterm = hs & 0x3;
	dphy->lp_rterm = lp & 0x3;
	dphy->have_trims = true;

	dev_info(dphy->dev, "eFuse trims: HS_RTERM %u, LP_RTERM %u\n",
		 dphy->hs_rterm, dphy->lp_rterm);

	return 0;
}

static int mstar_dphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct mstar_dphy *dphy;
	int i, ret;

	dphy = devm_kzalloc(dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;
	dphy->dev = dev;

	dphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dphy->base))
		return PTR_ERR(dphy->base);

	for (i = 0; i < DPHY_NCHANNELS; i++)
		dphy->lane_map[i] = i;
	ret = of_property_read_u8_array(dev->of_node, "mstar,lane-map",
					dphy->lane_map, DPHY_NCHANNELS);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret, "bad mstar,lane-map\n");
	for (i = 0; i < DPHY_NCHANNELS; i++)
		if (dphy->lane_map[i] >= DPHY_NCHANNELS)
			return dev_err_probe(dev, -EINVAL, "bad lane in mstar,lane-map\n");

	ret = mstar_dphy_read_trims(dphy);
	if (ret)
		return ret;

	dphy->phy = devm_phy_create(dev, NULL, &mstar_dphy_ops);
	if (IS_ERR(dphy->phy)) {
		dev_err(dev, "failed to create PHY\n");
		return PTR_ERR(dphy->phy);
	}

	phy_set_drvdata(dphy->phy, dphy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id mstar_dphy_of_match[] = {
	{
		.compatible	= "sstar,ssd20xd-dphy",
	},
	{ }
};

static struct platform_driver mstar_dphy_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_dphy_of_match,
	},
	.probe = mstar_dphy_probe,
};
module_platform_driver(mstar_dphy_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MIPI DPHY");
MODULE_LICENSE("GPL v2");
