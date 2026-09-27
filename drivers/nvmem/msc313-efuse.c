// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

struct msc313_efuse_priv {
	void __iomem *regs;
};

/*
 * The fuse array is read through the controller: bit 8 of the register at
 * 0x0c selects the bank (logical words 8..15 and 24..31 are in the second
 * bank), and each logical 32-bit word is a pair of 16-bit registers at the
 * row offsets below (low half, then +4 for the high half). This is the
 * layout the vendor IPL's reader uses, so cell offsets are word * 4.
 */
#define MSC313_EFUSE_BANK	0x0c
#define MSC313_EFUSE_BANK_SEL	BIT(8)

static const unsigned int msc313_efuse_row_off[8] = {
	0x10, 0x18, 0x20, 0x28, 0x58, 0x60, 0x68, 0x70,
};

static u16 msc313_efuse_read_half(struct msc313_efuse_priv *priv, unsigned int word, unsigned int half)
{
	void __iomem *bank = priv->regs + MSC313_EFUSE_BANK;
	void __iomem *row = priv->regs + msc313_efuse_row_off[word & 7] + half * 4;
	u16 ctrl = readw(bank);

	if (word & 8)
		writew(ctrl | MSC313_EFUSE_BANK_SEL, bank);
	else
		writew(ctrl & ~MSC313_EFUSE_BANK_SEL, bank);

	return readw(row);
}

static int msc313_efuse_reg_read(void *context, unsigned int reg, void *val, size_t bytes)
{
	struct msc313_efuse_priv *priv = context;
	u8 *out = val;

	if (reg & 1 || bytes & 1)
		return -EINVAL;

	while (bytes) {
		u16 half = msc313_efuse_read_half(priv, (reg / 4) & 0xf, (reg / 2) & 1);

		*out++ = half & 0xff;
		*out++ = half >> 8;
		reg += 2;
		bytes -= 2;
	}

	return 0;
}

static const struct of_device_id msc313_efuse_of_table[] = {
	{ .compatible = "mstar,msc313-efuse", },
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, msc313_efuse_of_table);

static int msc313_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct nvmem_device *nvmem;
	struct msc313_efuse_priv *priv;

	struct nvmem_config config = {
		.name = "msc313-efuse",
		.size = 0x80,
		.stride = 2,
		.word_size = 2,
		.reg_read = msc313_efuse_reg_read,
		.add_legacy_fixed_of_cells = true,
		.read_only = true,
		.root_only = true,
	};

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	config.dev = dev;
	config.priv = priv;

	nvmem = devm_nvmem_register(dev, &config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static struct platform_driver msc313_efuse_driver = {
	.probe = msc313_efuse_probe,
	.driver = {
		.name = "msc313-efuse",
		.of_match_table = msc313_efuse_of_table,
	},
};
module_platform_driver(msc313_efuse_driver);
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("MStar/SigmaStar EFUSE driver");
MODULE_LICENSE("GPL v2");
