/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME "mstar-pnl"

struct mstar_pnl {
	struct device *dev;
	struct regmap *regmap;
};

static const struct regmap_config mstar_pnl_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_pnl_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_pnl *pnl;
	struct resource *res;
	__iomem void *regs;
	struct platform_device *pdev = to_platform_device(dev);

	dev_info(dev, "binding");

	pnl = devm_kzalloc(dev, sizeof(*pnl), GFP_KERNEL);
	if(!pnl){
		return -ENOMEM;
	}

	pnl->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(IS_ERR(res))
		return PTR_ERR(res);

	regs = devm_ioremap_resource(dev, res);
	if(IS_ERR(regs))
		return PTR_ERR(regs);

	pnl->regmap = devm_regmap_init_mmio(dev, regs, &mstar_pnl_regmap_config);
	if(IS_ERR(pnl->regmap))
		return PTR_ERR(pnl->regmap);

	return 0;
}

static void mstar_pnl_unbind(struct device *dev, struct device *master,
			    void *data)
{
}

static const struct component_ops mstar_pnl_ops = {
	.bind	= mstar_pnl_bind,
	.unbind	= mstar_pnl_unbind,
};

static int mstar_pnl_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &mstar_pnl_ops);
}

static int mstar_pnl_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_pnl_ops);
	return 0;
}

static const struct of_device_id mstar_pnl_dt_ids[] = {
	{ .compatible = "mstar,pnl" },
	{},
};
MODULE_DEVICE_TABLE(of, mstar_pnl_dt_ids);

static struct platform_driver mstar_pnl_driver = {
	.probe = mstar_pnl_probe,
	.remove = mstar_pnl_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_pnl_dt_ids,
	},
};

module_platform_driver(mstar_pnl_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
