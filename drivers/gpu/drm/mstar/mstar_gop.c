/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/component.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>

#include "mstar_gop.h"

#define DRIVER_NAME "mstar-gop"

struct mstar_gop {
	struct device *dev;
	struct regmap *regmap;
	struct clk *fclk; /* vendor code says this is only needed when setting the palette */

	struct regmap_field *dst;
};

static const struct regmap_config mstar_gop_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static irqreturn_t mstar_gop_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static const char* dsts[] = {
		"ip_main", "ip_sub", "op", "mvop", "sub_mvop", "unknown", "frc", "unknown"
};

static void mstar_gop_dump(struct mstar_gop *gop){
	unsigned int val;
	regmap_field_read(gop->dst, &val);
	dev_info(gop->dev, "dst: %s", dsts[val]);
}


static void mstar_gop_reset(struct mstar_gop *gop){
	dev_info(gop->dev, "reset");
	regmap_update_bits(gop->regmap, MSTAR_GOP_REG_CONFIG,
			MSTAR_GOP_REG_CONFIG_RST, MSTAR_GOP_REG_CONFIG_RST);
	mdelay(10);
	regmap_update_bits(gop->regmap, MSTAR_GOP_REG_CONFIG,
			MSTAR_GOP_REG_CONFIG_RST, 0);

	mstar_gop_dump(gop);
}

static int mstar_gop_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_gop *gop;
	struct resource *res;
	__iomem void *regs;
	struct platform_device *pdev = to_platform_device(dev);
	int irq, ret;

	dev_info(dev, "binding");

	gop = devm_kzalloc(dev, sizeof(*gop), GFP_KERNEL);
	if(!gop){
		return -ENOMEM;
	}

	gop->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(IS_ERR(res))
		return PTR_ERR(res);

	regs = devm_ioremap_resource(dev, res);
	if(IS_ERR(regs))
		return PTR_ERR(regs);

	gop->regmap = devm_regmap_init_mmio(dev, regs, &mstar_gop_regmap_config);
	if(IS_ERR(gop->regmap))
		return PTR_ERR(gop->regmap);

	gop->dst = regmap_field_alloc(gop->regmap, gop_dst_field);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq){
		dev_err(gop->dev, "no interrupt provided");
		return -ENODEV;
	}
	else {
		ret = devm_request_irq(&pdev->dev, irq, mstar_gop_irq, IRQF_SHARED,
			dev_name(&pdev->dev), gop);
		if (ret)
			return ret;
	}

	gop->fclk = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR(gop->fclk)) {
		return PTR_ERR(gop->fclk);
	}

	mstar_gop_reset(gop);

	return ret;
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
