// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME "mstar-ge"

struct mstar_ge {
	int dummy;
};

static const struct regmap_config mstar_ge_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_ge_bind(struct device *dev, struct device *master,
			 void *data)
{
	return 0;
}

static void mstar_ge_unbind(struct device *dev, struct device *master,
			    void *data)
{
}


static const struct component_ops mstar_ge_component_ops = {
	.bind	= mstar_ge_bind,
	.unbind	= mstar_ge_unbind,
};

static int mstar_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_ge *ge;
	struct regmap *regmap;
	void __iomem *base;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_ge_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	dev_set_drvdata(dev, ge);

	return component_add(&pdev->dev, &mstar_ge_component_ops);
}

static int mstar_ge_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_ge_component_ops);
	return 0;
}

static const struct of_device_id mstar_ge_ids[] = {
	{
		.compatible = "sstar,ssd20xd-ge",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_ge_ids);

static struct platform_driver mstar_ge_driver = {
	.probe = mstar_ge_probe,
	.remove = mstar_ge_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_ge_ids,
	},
};
module_platform_driver(mstar_ge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
