/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HW RSA driver for MStar/SigmaStar MSC313 and later SoCs
 *
 * Copyright (C) 2021 Daniel Palmer <daniel@thingy.jp>
 */

#include <crypto/akcipher.h>
#include <crypto/internal/akcipher.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/of_irq.h>

#define DRIVER_NAME "msc313-rsa"

#define REG_CTRL	0x0
#define REG_IND32	0x4
#define REG_ADDR	0x8
#define REG_FILE_IN	0xc
#define REG_FILE_OUT	0x14
#define REG_START	0x1c
#define REG_KEYCONFIG	0x20
#define REG_STATUS	0x24

#define ADDR_E		0x00
#define ADDR_N		0x40
#define ADDR_A		0x80
#define ADDR_Z		0xc0

static struct list_head dev_list = LIST_HEAD_INIT(dev_list);
static spinlock_t dev_list_lock = __SPIN_LOCK_UNLOCKED(dev_list_lock);

static const struct regmap_config msc313_rsa_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4
};

struct msc313_rsa {
	struct device *dev;
	struct clk *clk;
	struct list_head dev_list;
	int irq;
	struct regmap *regmap;
	bool quirk_dummyread;

	struct regmap_field *reset;

	struct regmap_field *ind32start;
	struct regmap_field *write;
	struct regmap_field *autoinc;
	struct regmap_field *autostart;

	struct regmap_field *start;

	struct regmap_field *busy;
	struct regmap_field *done;

	/* key config */
	struct regmap_field *hwkey;
	struct regmap_field *pubkey;
	struct regmap_field *keylen;
};

static struct reg_field ctrl_ind32start = REG_FIELD(REG_CTRL, 0, 0);
static struct reg_field ind32_write = REG_FIELD(REG_IND32, 1, 1);
static struct reg_field ind32_autoinc = REG_FIELD(REG_IND32, 2, 2);
static struct reg_field ind32_autostart = REG_FIELD(REG_IND32, 3, 3);

static struct reg_field start_field_start = REG_FIELD(REG_START, 0, 0);

static struct reg_field keyconfig_reset = REG_FIELD(REG_KEYCONFIG, 0, 0);
static struct reg_field keyconfig_field_hw = REG_FIELD(REG_KEYCONFIG, 1, 1);
static struct reg_field keyconfig_field_public = REG_FIELD(REG_KEYCONFIG, 2, 2);
static struct reg_field keyconfig_field_length = REG_FIELD(REG_KEYCONFIG, 8, 13);

static struct reg_field status_field_busy = REG_FIELD(REG_STATUS, 0, 0);
static struct reg_field status_field_done = REG_FIELD(REG_STATUS, 1, 1);

static const struct of_device_id msc313_rsa_of_match[] = {
	{ .compatible = "mstar,msc313-rsa", },
	{ .compatible = "sstar,ssd20xd-rsa", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_rsa_of_match);

static struct msc313_rsa *msc313_rsa_find_dev(void)
{
	struct msc313_rsa *tmp = NULL;
	struct msc313_rsa *rsa = NULL;

	spin_lock_bh(&dev_list_lock);
		list_for_each_entry(tmp, &dev_list, dev_list) {
			rsa = tmp;
			break;
		}
	spin_unlock_bh(&dev_list_lock);

	return rsa;
}

static void msc313_rsa_reset(struct msc313_rsa *rsa)
{
	regmap_field_write(rsa->reset, 1);
	clk_prepare_enable(rsa->clk);
	mdelay(10);
	regmap_field_write(rsa->reset, 0);
	mdelay(10);
}

static irqreturn_t msc313_rsa_irq(int irq, void *data)
{
	struct msc313_rsa *rsa = data;
	return IRQ_HANDLED;
}

static int msc313_rsa_write_memory(struct msc313_rsa *rsa, u16 addr, const u8 *buffer, unsigned int len)
{
	unsigned int i, tmp;

	if (len % 4 != 0)
		return -EINVAL;

	regmap_write(rsa->regmap, REG_ADDR, addr);

	regmap_field_write(rsa->write, 1);
	regmap_field_write(rsa->autoinc, 1);
	regmap_field_write(rsa->autostart, 1);
	regmap_field_write(rsa->ind32start, 1);

	for(i = 0; i < len; i += 4){
		regmap_write(rsa->regmap, REG_FILE_IN, buffer[i] | (buffer[i + 1] << 8));
		if(rsa->quirk_dummyread)
			regmap_read(rsa->regmap, REG_FILE_IN, &tmp);
		regmap_write(rsa->regmap, REG_FILE_IN + 4, buffer[i + 2] | (buffer[i + 3] << 8));
		if(rsa->quirk_dummyread)
			regmap_read(rsa->regmap, REG_FILE_IN, &tmp);
	}
	regmap_field_write(rsa->ind32start, 0);

	return 0;
}

static int msc313_rsa_read_memory(struct msc313_rsa *rsa, u16 addr, u8 *buffer, unsigned int len)
{
	unsigned int i, tmp;

	if (len % 4 != 0)
		return -EINVAL;

	regmap_write(rsa->regmap, REG_ADDR, addr);

	regmap_field_write(rsa->write, 0);
	regmap_field_write(rsa->autoinc, 1);
	regmap_field_write(rsa->autostart, 1);
	regmap_field_write(rsa->ind32start, 1);

	for(i = 0; i < len; i += 4){
		regmap_read(rsa->regmap, REG_FILE_OUT, &tmp);
		if(rsa->quirk_dummyread)
			regmap_read(rsa->regmap, REG_FILE_OUT, &tmp);
		buffer[i] = tmp;
		buffer[i + 1] = tmp >> 8;
		regmap_read(rsa->regmap, REG_FILE_OUT + 4, &tmp);
		if(rsa->quirk_dummyread)
			regmap_read(rsa->regmap, REG_FILE_OUT + 4, &tmp);
		buffer[i + 2] = tmp;
		buffer[i + 3] = tmp >> 8;
	}

	regmap_field_write(rsa->ind32start, 0);

	dev_info(rsa->dev, "out: %64phN\n", buffer);

	return 0;
}

static int msc313_rsa_do_one(struct msc313_rsa *rsa, u8 *out, unsigned int len)
{
	unsigned int done;
	int ret = 0;

	regmap_field_write(rsa->start, 1);

	if (regmap_field_read_poll_timeout(rsa->done, done, done == 1, 1, 500000)) {
		dev_err(rsa->dev, "timeout waiting for rsa to finish\n");
		ret = -ETIMEDOUT;
	}

	msc313_rsa_read_memory(rsa, ADDR_Z, out, len);

	regmap_field_write(rsa->start, 0);



	return ret;
}

static void msc313_rsa_test(struct msc313_rsa *rsa)
{
	u8 i;
	u8 test_in[64] = { };
	u8 test_out[64] = { };

	for(i = 0; i < ARRAY_SIZE(test_in); i++)
		test_in[i] = ~i;

	msc313_rsa_write_memory(rsa, 0, test_in, ARRAY_SIZE(test_in));

	dev_info(rsa->dev, "in: %64phN\n", test_in);

	msc313_rsa_do_one(rsa, test_out, sizeof(test_out));
}

static int msc313_rsa_sign(struct akcipher_request *req)
{
	printk("%s\n", __func__);

	return 0;
}

static int msc313_rsa_verify(struct akcipher_request *req)
{
	printk("%s\n", __func__);

	return 0;
}

static int msc313_rsa_encrypt(struct akcipher_request *req)
{
	struct msc313_rsa *rsa = msc313_rsa_find_dev();
	printk("%s\n", __func__);
	u8 test_out[64] = { };

	msc313_rsa_do_one(rsa, test_out, sizeof(test_out));

	return 0;
}

static int msc313_rsa_decrypt(struct akcipher_request *req)
{
	struct msc313_rsa *rsa = msc313_rsa_find_dev();
	printk("%s\n", __func__);

	return 0;
}

static int msc313_rsa_set_pub_key(struct crypto_akcipher *tfm, const void *key,
		   unsigned int keylen)
{
	struct msc313_rsa *rsa = msc313_rsa_find_dev();
	printk("%s, %u\n", __func__, keylen);

	regmap_field_write(rsa->pubkey, 1);

	return 0;
}
static int msc313_rsa_set_priv_key(struct crypto_akcipher *tfm, const void *key,
		    unsigned int keylen)
{
	struct msc313_rsa *rsa = msc313_rsa_find_dev();
	printk("%s, %u\n", __func__, keylen);

	regmap_field_write(rsa->pubkey, 0);

	return 0;
}

static unsigned int msc313_rsa_max_size(struct crypto_akcipher *tfm)
{
	printk("%s\n", __func__);

	return 64;
}

static struct akcipher_alg rsa_alg = {
	.sign = msc313_rsa_sign,
	.verify = msc313_rsa_verify,
	.encrypt = msc313_rsa_encrypt,
	.decrypt = msc313_rsa_decrypt,
	.set_pub_key = msc313_rsa_set_pub_key,
	.set_priv_key = msc313_rsa_set_priv_key,
	.max_size = msc313_rsa_max_size,
	.base = {
		.cra_name = "rsa",
		.cra_driver_name = "msc313-rsa",
		.cra_priority = 3000,
		.cra_module = THIS_MODULE,
	},
};

static int msc313_rsa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msc313_rsa *rsa;
	void __iomem *base;
	int i, ret, irq;

	rsa = devm_kzalloc(&pdev->dev, sizeof(*rsa), GFP_KERNEL);
	if (!rsa)
		return -ENOMEM;

	rsa->dev = dev;

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

	rsa->start = devm_regmap_field_alloc(dev, rsa->regmap, start_field_start);

	rsa->busy = devm_regmap_field_alloc(dev, rsa->regmap, status_field_busy);
	rsa->done = devm_regmap_field_alloc(dev, rsa->regmap, status_field_done);

	/* keyconfig */
	rsa->hwkey = devm_regmap_field_alloc(dev, rsa->regmap, keyconfig_field_hw);
	rsa->pubkey = devm_regmap_field_alloc(dev, rsa->regmap, keyconfig_field_public);
	rsa->keylen = devm_regmap_field_alloc(dev, rsa->regmap, keyconfig_field_length);

	rsa->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(rsa->clk)) {
		return PTR_ERR(rsa->clk);
	}

	/*irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
		if (!irq)
			return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_rsa_irq, IRQF_SHARED,
			dev_name(&pdev->dev), rsa);*/

	msc313_rsa_reset(rsa);

	msc313_rsa_test(rsa);

	spin_lock(&dev_list_lock);
	list_add_tail(&rsa->dev_list, &dev_list);
	spin_unlock(&dev_list_lock);

	crypto_register_akcipher(&rsa_alg);

	return 0;
}

static int msc313_rsa_remove(struct platform_device *pdev)
{
	crypto_unregister_akcipher(&rsa_alg);
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

MODULE_DESCRIPTION("MStar MSC313 RSA driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
