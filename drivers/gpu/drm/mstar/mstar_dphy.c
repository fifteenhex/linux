/*
 *  0x0
 *    6    |   0
 *  pd_ldo | sw_rst
 *
 *  0x4
 *               1               |          0
 *  power down whole dphy analog | power down hs mode
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>

#define DRIVER_NAME "mstar-mipi_dphy"

static int mstar_dphy_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id mstar_dphy_of_match[] = {
	{
		.compatible	= "mstar,mipi_dphy",
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
