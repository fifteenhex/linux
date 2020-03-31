/* SPDX-License-Identifier: GPL-2.0-or-later */
#define DRIVER_NAME "mstar-gop"

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include "mstar-gop.h"

struct mstar_gop {
	struct device *dev;
	struct regmap *regmap;
};

static const struct regmap_config mstar_gop_regmap_config = {
		.name = "mstar-gop",
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4,
};

static int mstar_gop_probe(struct platform_device *pdev)
{
	struct mstar_gop *gop;
	struct resource *mem;
	__iomem void *base;

	gop = devm_kzalloc(&pdev->dev, sizeof(*gop), GFP_KERNEL);
	if(!gop){
		return -ENOMEM;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	gop->regmap = devm_regmap_init_mmio(&pdev->dev, base,
			&mstar_gop_regmap_config);
	if(IS_ERR(gop->regmap)){
		return PTR_ERR(gop->regmap);
	}

	return 0;
}

static int mstar_gop_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id mstar_gop_dt_ids[] = {
	{ .compatible = "mstar,gop" },
	{},
};
MODULE_DEVICE_TABLE(of, mstar_gop_dt_ids);

static struct platform_driver mstar_gop_driver = {
	.probe = mstar_gop_probe,
	.remove = mstar_gop_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_gop_dt_ids,
	},
};

module_platform_driver(mstar_gop_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mstar GOP Driver");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
