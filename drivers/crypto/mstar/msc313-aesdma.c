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
 *
 * RSA unit
 *
 * 0x9C - rsa int clr?
 * 0xA0 - rsa ctrl?
 *  0
 * rst
 */

#define DRIVER_NAME "msc313-aesdma"

#define REG_CTRL0	0x0
#define REG_CTRL1	0x4
#define REG_SRC		0x8
#define REG_XIU_LEN	0x10
#define REG_DST_START	0x18
#define REG_DST_END	0x20

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
	struct regmap_field *start;
	struct regmap_field *reset;
	struct regmap_field *aes;
	struct regmap_field *fout;
};

static struct reg_field ctrl_fstart = REG_FIELD(REG_CTRL0, 0, 0);
static struct reg_field ctrl_reset = REG_FIELD(REG_CTRL0, 7, 7);
static struct reg_field ctrl_fout = REG_FIELD(REG_CTRL0, 8, 8);
static struct reg_field ctrl_aes = REG_FIELD(REG_CTRL1, 8, 8);


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

static volatile u8 test_in[128] = { };
static volatile u8 test_out[128] = { };

static void msc313_aesdma_test(struct msc313_aesdma *aesdma)
{
	unsigned src = virt_to_phys(test_in) - 0x20000000;
	unsigned dst = virt_to_phys(test_out) - 0x20000000;
	unsigned len = sizeof(test_in);
	unsigned end = dst + len - 1;
	int i;

	for(i = 0; i < ARRAY_SIZE(test_out); i++)
		test_out[i] = 0;
	mb();

	clk_prepare_enable(aesdma->clk);

	regmap_field_write(aesdma->reset, 0);

	regmap_field_write(aesdma->aes, 1);
	regmap_field_write(aesdma->fout, 1);

	regmap_write(aesdma->regmap, REG_SRC, src);
	regmap_write(aesdma->regmap, REG_SRC + 4, src >> 16);

	regmap_write(aesdma->regmap, REG_XIU_LEN, len);
	regmap_write(aesdma->regmap, REG_XIU_LEN + 4, len >> 16);

	regmap_write(aesdma->regmap, REG_DST_START, dst);
	regmap_write(aesdma->regmap, REG_DST_START + 4, dst >> 16);

	regmap_write(aesdma->regmap, REG_DST_END, end);
	regmap_write(aesdma->regmap, REG_DST_END + 4, end >> 16);

	regmap_field_write(aesdma->start, 1);

	mdelay(100);

	mb();
	for(i = 0; i < ARRAY_SIZE(test_out); i++)
		printk("in: %02x out: %02x\n", (unsigned) test_in[i], (unsigned) test_out[i]);
}

static int msc313_aesdma_probe(struct platform_device *pdev)
{
	struct msc313_aesdma *aesdma;
	void __iomem *base;
	int i, ret, irq;

	aesdma = devm_kzalloc(&pdev->dev, sizeof(*aesdma), GFP_KERNEL);
	if (!aesdma)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		return PTR_ERR(base);
	}

	aesdma->regmap = devm_regmap_init_mmio(&pdev->dev, base,
                        &msc313_aesdma_regmap_config);
	if(IS_ERR(aesdma->regmap)){
		dev_err(&pdev->dev, "failed to register regmap");
		return PTR_ERR(aesdma->regmap);
	}

	aesdma->start = devm_regmap_field_alloc(&pdev->dev, aesdma->regmap, ctrl_fstart);
	aesdma->reset = devm_regmap_field_alloc(&pdev->dev, aesdma->regmap, ctrl_reset);
	aesdma->fout = devm_regmap_field_alloc(&pdev->dev, aesdma->regmap, ctrl_fout);
	aesdma->aes = devm_regmap_field_alloc(&pdev->dev, aesdma->regmap, ctrl_aes);

	regmap_field_write(aesdma->reset, 1);

	aesdma->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(aesdma->clk)) {
		return PTR_ERR(aesdma->clk);
	}

	/*irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
		if (!irq)
			return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_aesdma_irq, IRQF_SHARED,
			dev_name(&pdev->dev), aesdma);*/

	msc313_aesdma_test(aesdma);

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
