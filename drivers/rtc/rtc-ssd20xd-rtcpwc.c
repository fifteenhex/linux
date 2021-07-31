// SPDX-License-Identifier: GPL-2.0-only
/*
 * (C) 2021 Daniel Palmer
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/regmap.h>

struct reg_field rddata_l_field = REG_FIELD(0x24, 0, 15);
struct reg_field rddata_h_field = REG_FIELD(0x28, 0, 15);

struct ssd20xd_rtcpwc {
	struct rtc_device *rtc_dev;
	struct regmap_field *rddata_l, *rddata_h;
};

static int ssd20xd_rtcpwc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct ssd20xd_rtcpwc *priv = dev_get_drvdata(dev);
	unsigned l, h;
	u32 seconds;

	regmap_field_read(priv->rddata_l, &l);
	regmap_field_read(priv->rddata_h, &h);

	seconds = h << 16 | l;

	rtc_time64_to_tm(seconds, tm);

	return 0;
}

static int ssd20xd_rtcpwc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct ssd20xd_rtcpwc *priv = dev_get_drvdata(dev);
	unsigned long seconds;
	u16 reg;

#if 0
	seconds = rtc_tm_to_time64(tm);
	writew(seconds & 0xFFFF, priv->rtc_base + REG_RTC_LOAD_VAL_L);
	writew((seconds >> 16) & 0xFFFF, priv->rtc_base + REG_RTC_LOAD_VAL_H);
	reg = readw(priv->rtc_base + REG_RTC_CTRL);
	writew(reg | LOAD_EN_BIT, priv->rtc_base + REG_RTC_CTRL);

	/* need to check carefully if we want to clear REG_RTC_LOAD_VAL_H for customer*/
	while (readw(priv->rtc_base + REG_RTC_CTRL) & LOAD_EN_BIT)
		udelay(1);
	writew(0, priv->rtc_base + REG_RTC_LOAD_VAL_H);
#endif

	return 0;
}

static const struct rtc_class_ops ssd20xd_rtcpwc_ops = {
	.read_time = ssd20xd_rtcpwc_read_time,
	.set_time = ssd20xd_rtcpwc_set_time,
};

static const struct regmap_config ssd20xd_rtcpwc_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int ssd20xd_rtcpwc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ssd20xd_rtcpwc *priv;
	struct regmap *regmap;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct ssd20xd_rtcpwc), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &ssd20xd_rtcpwc_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	priv->rddata_l = devm_regmap_field_alloc(dev, regmap, rddata_l_field);
	if(IS_ERR(priv->rddata_l))
		return PTR_ERR(priv->rddata_l);

	priv->rddata_h = devm_regmap_field_alloc(dev, regmap, rddata_h_field);
	if(IS_ERR(priv->rddata_h))
		return PTR_ERR(priv->rddata_h);

	platform_set_drvdata(pdev, priv);

	priv->rtc_dev = devm_rtc_device_register(dev, dev_name(dev), &ssd20xd_rtcpwc_ops, THIS_MODULE);
	if (IS_ERR(priv->rtc_dev))
		return PTR_ERR(priv->rtc_dev);

	return 0;
}

static const struct of_device_id ssd20xd_rtcpwc_of_match_table[] = {
	{ .compatible = "sstar,ssd20xd-rtcpwc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ms_rtc_of_match_table);

static struct platform_driver ssd20xd_rtcpwc_driver = {
	.probe = ssd20xd_rtcpwc_probe,
	.driver = {
		.name = "ssd20xd-rtcpwc",
		.of_match_table = ssd20xd_rtcpwc_of_match_table,
	},
};

module_platform_driver(ssd20xd_rtcpwc_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("MStar RTC PWC Driver");
MODULE_LICENSE("GPL v2");
