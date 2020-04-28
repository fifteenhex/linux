// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define MSC313_PHY_ID	0xdeadbeef
#define MSC313E_PHY_ID	0xdeadb33f
#define MSC313_PHY_MASK 0xffffffff

#define REG_LDO		0x3f8

struct msc313_phy_data;

struct msc313_phy_priv {
	struct regmap *phyana;
	const struct msc313_phy_data *data;
};

struct msc313_phy_data {
	void (*powerup)(struct msc313_phy_priv*);
	void (*powerdown)(struct msc313_phy_priv*);
};

static void msc313_powerdown(struct msc313_phy_priv *priv)
{
	printk("Doing phy power down\n");
	regmap_write(priv->phyana, REG_LDO, 0xffff);
};

static void msc313_powerup(struct msc313_phy_priv *priv){
	printk("Doing phy power up\n");
	regmap_write(priv->phyana, REG_LDO, 0x0);
}

static const struct msc313_phy_data msc313_data = {
	.powerup = msc313_powerup,
	.powerdown = msc313_powerdown,
};

static void msc313e_powerdown(struct msc313_phy_priv *priv)
{
	printk("Doing phy power down\n");
	regmap_write(priv->phyana, REG_LDO, 0xffff);
};

static void msc313e_powerup(struct msc313_phy_priv *priv){
	printk("Doing phy power up\n");
	regmap_write(priv->phyana, REG_LDO, 0x0);
}

static const struct msc313_phy_data msc313e_data = {
	.powerup = msc313e_powerup,
	.powerdown = msc313e_powerdown,
};

static int msc313_phy_suspend(struct phy_device *phydev)
{
	struct msc313_phy_priv *priv = phydev->priv;
	priv->data->powerdown(priv);
	return 0;
}

static int msc313_phy_resume(struct phy_device *phydev)
{
	struct msc313_phy_priv *priv = phydev->priv;
	priv->data->powerup(priv);
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

	priv->data = phydev->drv->driver_data;

	phydev->priv = priv;
out:
	return ret;
}

struct phy_driver msc313_driver[] = {
	{
		.phy_id         = MSC313_PHY_ID,
		.phy_id_mask    = 0xffffffff,
		.name           = "msc313 phy",
		.probe		= msc313_phy_probe,
		.soft_reset	= genphy_no_soft_reset,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
		.driver_data	= &msc313_data,
	},
	{
		.phy_id         = MSC313E_PHY_ID,
		.phy_id_mask    = 0xffffffff,
		.name           = "msc313e phy",
		.probe		= msc313_phy_probe,
		.soft_reset	= genphy_no_soft_reset,
		.suspend	= msc313_phy_suspend,
		.resume		= msc313_phy_resume,
		.driver_data	= &msc313e_data,
	},
};

module_phy_driver(msc313_driver);

static struct mdio_device_id __maybe_unused msc313_tbl[] = {
	{ MSC313_PHY_ID, MSC313_PHY_MASK },
	{ MSC313E_PHY_ID, MSC313_PHY_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, msc313_tbl);
