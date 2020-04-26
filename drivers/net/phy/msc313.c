// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define MSC313_PHY_ID	0xdeadbeef
#define MSC313_PHY_MASK 0xffffffff

#define REG_LDO_0	0x3f8
#define REG_LDO_1	0x3f9

/*
	     // power on adc
	     *(int8_t *)0x1f006741 = 0x80;
	     // power on bgap
	     *(int8_t *)0x1f006598 = 0x40;
	     // power on adcpl
	     *(int8_t *)0x1f006575 = 0x4;
	     *(int8_t *)0x1f006674 = 0x0;
	     // power on lpf_op
	     *(int8_t *)0x1f0067e1 = 0x0;
	     */


struct msc313_phy_priv {
	struct regmap *phyana;
};

static void msc313_phy_powerdown(struct msc313_phy_priv *priv)
{
	printk("Doing phy power down\n");
	regmap_write(priv->phyana, REG_LDO_0, 0xffff);
	regmap_write(priv->phyana, REG_LDO_1, 0xffff);
}

static void msc313_phy_powerup(struct msc313_phy_priv *priv){
	printk("Doing phy power up\n");
	regmap_write(priv->phyana, REG_LDO_0, 0x0);
	regmap_write(priv->phyana, REG_LDO_1, 0x0);
}

static int msc313_phy_suspend(struct phy_device *phydev)
{
	msc313_phy_powerdown(phydev->priv);
	return 0;
}

static int msc313_phy_resume(struct phy_device *phydev)
{
	msc313_phy_powerup(phydev->priv);
	return 0;
}

static int msc313_phy_probe(struct phy_device *phydev)
{
	struct device_node *of_node = phydev->mdio.dev.of_node;
	struct msc313_phy_priv *priv;
	int ret = 0;

	printk("phy probe\n");

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if(IS_ERR(priv)){
		ret = PTR_ERR(priv);
		goto out;
	}

	priv->phyana = syscon_regmap_lookup_by_phandle(of_node, "mstar,phyana");
	if(IS_ERR(priv->phyana)){
		ret = PTR_ERR(priv->phyana);
		goto out;
	}

	phydev->priv = priv;
out:
	return ret;
}

struct phy_driver msc313_driver[] = {
	{
		.phy_id         = MSC313_PHY_ID,
		.phy_id_mask    = 0xffffffff,
		.name           = "MStar 313 phy",
		.probe		= msc313_phy_probe,
		.soft_reset	= genphy_no_soft_reset,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
	},
};

module_phy_driver(msc313_driver);

static struct mdio_device_id __maybe_unused msc313_tbl[] = {
	{ MSC313_PHY_ID, MSC313_PHY_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, msc313_tbl);
