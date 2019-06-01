// SPDX-License-Identifier: GPL-2.0
//

#include <linux/debugfs.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/of_irq.h>

/*
 * SHARNG unit
 *
 * 0x20         - ctrl
 *  15   | 11  |    9    |  7  |  6  |   2   |  1
 *manual |  ?  | sha256  | rst | clr | busy  | ready
 * 0x28         - message address, low
 * 0x2c         - message address, high
 * 0x30         - message size, low
 * 0x34         - message size, high
 * 0x3c         - status
 * 0x40 .. 0x80 - output buffer
 *
 * RSA unit
 *
 * 0x9C - rsa int clr?
 * 0xA0 - rsa ctrl?
 *  0
 * rst
 *
 * AESDMA unit
 * 0x140        - aesdma ctrl 1?
 * 0x144        - aesdma ctrl 2?
 * 0x148 		- file in addr low?
 * 0x14c 		- file in addr high?
 * 0x158		- file out addr low ?
 * 0x15c        - file out addr high?
 * 0x160        - file size low?
 * 0x164        - file size high?
 * 0x19c .. 1b8 - key
 * 0x1bc .. 1d8 - iv
 * 0x1e4        - key type?
 */

#define DRIVER_NAME "msc313-aesdma"

static const struct regmap_config msc313_aesdma_regmap_config = {
		.name = DRIVER_NAME,
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4
};

struct msc313_aesdma {
	struct clk *clk;
	int irq;
	struct regmap *regmap;
};

static const struct of_device_id msc313_aesdma_of_match[] = {
	{ .compatible = "mstar,msc313-aesdma", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_aesdma_of_match);

static irqreturn_t msc313_aesdma_irq(int irq, void *data)
{
	struct msc313_aesdma *aesdma = data;
	return IRQ_HANDLED;
}

static int msc313_aesdma_probe(struct platform_device *pdev)
{
	struct msc313_aesdma *aesdma;
	struct resource *res;
	void __iomem *base;
	int i, ret, irq;

	aesdma = devm_kzalloc(&pdev->dev, sizeof(*aesdma), GFP_KERNEL);
	if (!aesdma)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		return PTR_ERR(base);
	}

	aesdma->regmap = devm_regmap_init_mmio(&pdev->dev, base,
                        &msc313_aesdma_regmap_config);
	if(IS_ERR(aesdma->regmap)){
		dev_err(&pdev->dev, "failed to register regmap");
		return PTR_ERR(aesdma->regmap);
	}

	aesdma->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(aesdma->clk)) {
		return PTR_ERR(aesdma->clk);
	}

	ret = clk_prepare_enable(aesdma->clk);
	if (ret) {
		dev_err(&pdev->dev, "clk enable failed: %d\n", ret);
	}

	/*irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
		if (!irq)
			return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_aesdma_irq, IRQF_SHARED,
			dev_name(&pdev->dev), aesdma);*/

	return 0;
}

static int msc313_aesdma_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313_aesdma_driver = {
	.probe = msc313_aesdma_probe,
	.remove = msc313_aesdma_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_aesdma_of_match,
	},
};

module_platform_driver(msc313_aesdma_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 AESDMA driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
