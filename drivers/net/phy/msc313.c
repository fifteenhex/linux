// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/phy.h>

#define MSC313_PHY_ID	0xdeadbeef
#define MSC313_PHY_MASK 0xffffffff

int msc313_phy_probe(struct phy_device *phydev){
	return 0;
}

struct phy_driver msc313_driver[] = {
		{
				.phy_id         = MSC313_PHY_ID,
				.phy_id_mask    = 0xffffffff,
				.name           = "MStar 313 phy",
				.probe			= msc313_phy_probe,
				.soft_reset		= genphy_no_soft_reset,
		},
};

module_phy_driver(msc313_driver);

static struct mdio_device_id __maybe_unused msc313_tbl[] = {
	{ MSC313_PHY_ID, MSC313_PHY_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, msc313_tbl);
