/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>

#include "mstar_gop.h"

#define DRIVER_NAME "mstar-gop"

struct mstar_gop {
	struct device *dev;
};

static int mstar_gop_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_gop *gop;

	dev_info(dev, "binding");

	gop = devm_kzalloc(dev, sizeof(*gop), GFP_KERNEL);
	if(!gop){
		return -ENOMEM;
	}

	gop->dev = dev;
	return 0;
}

static void mstar_gop_unbind(struct device *dev, struct device *master,
			    void *data)
{
}

static const struct component_ops mstar_gop_ops = {
	.bind	= mstar_gop_bind,
	.unbind	= mstar_gop_unbind,
};

static int mstar_gop_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &mstar_gop_ops);
}

static int mstar_gop_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_gop_ops);
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
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
