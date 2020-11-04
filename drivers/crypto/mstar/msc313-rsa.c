// SPDX-License-Identifier: GPL-2.0
//

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

#define DRIVER_NAME "msc313-rsa"

#define REG_CTRL	0x0
#define REG_IND32	0x4
#define REG_ADDR	0x8
#define REG_DATA	0xc
#define REG_KEYCONFIG	0x20

static const struct regmap_config msc313_rsa_regmap_config = {
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4
};

struct msc313_rsa {
	struct clk *clk;
	int irq;
	struct regmap *regmap;

	struct regmap_field *reset;

	struct regmap_field *ind32start;
	struct regmap_field *write;
	struct regmap_field *autoinc;
	struct regmap_field *autostart;
};

static struct reg_field ctrl_ind32start = REG_FIELD(REG_CTRL, 0, 0);
static struct reg_field ind32_write = REG_FIELD(REG_IND32, 1, 1);
static struct reg_field ind32_autoinc = REG_FIELD(REG_IND32, 2, 2);
static struct reg_field ind32_autostart = REG_FIELD(REG_IND32, 3, 3);
static struct reg_field keyconfig_reset = REG_FIELD(REG_KEYCONFIG, 0, 0);

static const struct of_device_id msc313_rsa_of_match[] = {
	{ .compatible = "mstar,msc313-rsa", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_rsa_of_match);

static irqreturn_t msc313_rsa_irq(int irq, void *data)
{
	struct msc313_rsa *rsa = data;
	return IRQ_HANDLED;
}

static int msc313_rsa_write_memory(struct msc313_rsa *rsa, u16 addr, const u8 *buffer, unsigned int len)
{
	unsigned int i;

	if (len % 4 != 0)
		return -EINVAL;

	regmap_field_write(rsa->write, 1);
	regmap_field_write(rsa->autoinc, 1);
	regmap_field_write(rsa->autostart, 1);
	regmap_field_write(rsa->ind32start, 1);
	regmap_write(rsa->regmap, REG_ADDR, addr);

	for(i = 0; i < len; i += 4){
		regmap_write(rsa->regmap, REG_DATA, buffer[i] | (buffer[i + 1] << 8));
		regmap_write(rsa->regmap, REG_DATA + 4, buffer[i + 2] | (buffer[i + 3] << 8));
	}
	regmap_field_write(rsa->ind32start, 0);

	return 0;
}

static int msc313_rsa_read_memory(struct msc313_rsa *rsa, u16 addr, u8 *buffer, unsigned int len)
{
	unsigned int i, tmp;

	if (len % 4 != 0)
		return -EINVAL;

	regmap_field_write(rsa->write, 0);
	regmap_field_write(rsa->autoinc, 1);
	regmap_field_write(rsa->autostart, 1);
	regmap_write(rsa->regmap, REG_ADDR, addr);
	regmap_field_write(rsa->ind32start, 1);

	for(i = 0; i < len; i += 4){
		regmap_read(rsa->regmap, REG_ADDR, &tmp);
		printk("%u %u\n", i, tmp);
		regmap_read(rsa->regmap, REG_DATA, &tmp);
		printk("l %u\n", tmp);
		buffer[i] = tmp;
		buffer[i + 1] = tmp >> 8;
		regmap_read(rsa->regmap, REG_DATA + 4, &tmp);
		printk("l %u\n", tmp);
		buffer[i + 2] = tmp;
		buffer[i + 3] = tmp >> 8;
	}

	regmap_field_write(rsa->ind32start, 0);

	return 0;
}

static volatile u8 test_in[64] = { };
static volatile u8 test_out[64] = { };


static void msc313_rsa_test(struct msc313_rsa *rsa)
{
	u8 i;

	regmap_field_write(rsa->reset, 1);
	clk_prepare_enable(rsa->clk);
	mdelay(10);
	regmap_field_write(rsa->reset, 0);
	mdelay(10);

	for(i = 0; i < ARRAY_SIZE(test_in); i++)
		test_in[i] = ~i;

	msc313_rsa_write_memory(rsa, 0, test_in, ARRAY_SIZE(test_in));

	msc313_rsa_read_memory(rsa, 0, test_out, ARRAY_SIZE(test_out));

	for(i = 0; i < ARRAY_SIZE(test_in); i++)
		printk("rsa: %d %x:%x\n", (int)i, (unsigned) test_in[i], (unsigned) test_out[i]);

}

static int msc313_rsa_probe(struct platform_device *pdev)
{
	struct msc313_rsa *rsa;
	void __iomem *base;
	int i, ret, irq;

	rsa = devm_kzalloc(&pdev->dev, sizeof(*rsa), GFP_KERNEL);
	if (!rsa)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		return PTR_ERR(base);
	}

	rsa->regmap = devm_regmap_init_mmio(&pdev->dev, base,
                        &msc313_rsa_regmap_config);
	if(IS_ERR(rsa->regmap)){
		dev_err(&pdev->dev, "failed to register regmap");
		return PTR_ERR(rsa->regmap);
	}

	rsa->reset = devm_regmap_field_alloc(&pdev->dev, rsa->regmap, keyconfig_reset);

	rsa->ind32start = devm_regmap_field_alloc(&pdev->dev, rsa->regmap, ctrl_ind32start);
	rsa->write = devm_regmap_field_alloc(&pdev->dev, rsa->regmap, ind32_write);
	rsa->autoinc = devm_regmap_field_alloc(&pdev->dev, rsa->regmap, ind32_autoinc);
	rsa->autostart = devm_regmap_field_alloc(&pdev->dev, rsa->regmap, ind32_autostart);

	rsa->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(rsa->clk)) {
		return PTR_ERR(rsa->clk);
	}

	/*irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
		if (!irq)
			return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_rsa_irq, IRQF_SHARED,
			dev_name(&pdev->dev), rsa);*/

	msc313_rsa_test(rsa);

	return 0;
}

static int msc313_rsa_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313_rsa_driver = {
	.probe = msc313_rsa_probe,
	.remove = msc313_rsa_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_rsa_of_match,
	},
};

module_platform_driver(msc313_rsa_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 RSA driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
