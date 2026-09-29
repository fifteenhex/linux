// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME "mstar-moprot"

struct mstar_moprot {
	struct regmap *regmap;
};

/*
 * The MOP output stage. Without this the MOP's windows can be programmed and
 * enabled and the plane still never reaches the display mixer, so nothing of it
 * appears on the panel - which is why the graphics plane looked dead in Linux
 * while the GOP worked.
 *
 * Four channels 0x40 apart, each given the same three values, then the enable
 * in two steps. Taken from the vendor bring-up and not fully decoded, so it is
 * replayed verbatim; the same block is in u-boot's mstar_mop.c (mop_output[]),
 * where it is what makes the MOP path work.
 */
static const struct reg_sequence mstar_moprot_output_init[] = {
	{ 0x048, 0x0820 }, { 0x04c, 0xa01f }, { 0x058, 0x0801 },
	{ 0x088, 0x0820 }, { 0x08c, 0xa01f }, { 0x098, 0x0801 },
	{ 0x0c8, 0x0820 }, { 0x0cc, 0xa01f }, { 0x0d8, 0x0801 },
	{ 0x108, 0x0820 }, { 0x10c, 0xa01f }, { 0x118, 0x0801 },
	{ 0x004, 0x0001 }, { 0x004, 0x0003 },
};

static const struct regmap_config mstar_moprot_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_moprot_bind(struct device *dev, struct device *master,
			 void *data)
{
	return 0;
}

static void mstar_moprot_unbind(struct device *dev, struct device *master,
			    void *data)
{
}


static const struct component_ops mstar_moprot_component_ops = {
	.bind	= mstar_moprot_bind,
	.unbind	= mstar_moprot_unbind,
};

static int mstar_moprot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_moprot *moprot;
	struct regmap *regmap;
	void __iomem *base;
	int ret;

	moprot = devm_kzalloc(dev, sizeof(*moprot), GFP_KERNEL);
	if (!moprot)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_moprot_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	moprot->regmap = regmap;

	ret = regmap_multi_reg_write(regmap, mstar_moprot_output_init,
				     ARRAY_SIZE(mstar_moprot_output_init));
	if (ret)
		return ret;

	dev_set_drvdata(dev, moprot);

	return component_add(&pdev->dev, &mstar_moprot_component_ops);
}

static void mstar_moprot_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_moprot_component_ops);
}

/* the sleep reset clears the routing, so put it back before the mode set */
static int mstar_moprot_resume_noirq(struct device *dev)
{
	struct mstar_moprot *moprot = dev_get_drvdata(dev);

	return regmap_multi_reg_write(moprot->regmap, mstar_moprot_output_init,
				      ARRAY_SIZE(mstar_moprot_output_init));
}

static const struct dev_pm_ops mstar_moprot_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, mstar_moprot_resume_noirq)
};

static const struct of_device_id mstar_moprot_ids[] = {
	{
		.compatible = "sstar,ssd20xd-moprot",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_moprot_ids);

static struct platform_driver mstar_moprot_driver = {
	.probe = mstar_moprot_probe,
	.remove = mstar_moprot_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_moprot_ids,
		.pm = pm_sleep_ptr(&mstar_moprot_pm_ops),
	},
};
module_platform_driver(mstar_moprot_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
