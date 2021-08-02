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

struct reg_field base_wr_field = REG_FIELD(0x0, 1, 1);
struct reg_field base_rd_field = REG_FIELD(0x0, 2, 2);
struct reg_field iso_ctrl_field = REG_FIELD(0xc, 0, 2);
struct reg_field wrdata_l_field = REG_FIELD(0x10, 0, 15);
struct reg_field wrdata_h_field = REG_FIELD(0x14, 0, 15);
struct reg_field iso_ctrl_ack_field = REG_FIELD(0x20, 3, 3);
struct reg_field rddata_l_field = REG_FIELD(0x24, 0, 15);
struct reg_field rddata_h_field = REG_FIELD(0x28, 0, 15);
struct reg_field cnt_updating_field = REG_FIELD(0x2c, 0, 0);
struct reg_field rddata_cnt_l_field = REG_FIELD(0x30, 0, 15);
struct reg_field rddata_cnt_h_field = REG_FIELD(0x34, 0, 15);
struct reg_field cnt_rd_trig_field = REG_FIELD(0x38, 0, 0);
struct reg_field rst_field = REG_FIELD(0x40, 8, 8);
struct reg_field iso_en_field = REG_FIELD(0x54, 0, 0);

struct ssd20xd_rtcpwc {
	struct rtc_device *rtc_dev;
	struct regmap_field *base_wr, *base_rd;
	struct regmap_field *iso_ctrl, *iso_ctrl_ack, *iso_en;
	struct regmap_field *wrdata_l, *wrdata_h;
	struct regmap_field *rddata_l, *rddata_h;
	struct regmap_field *cnt_updating, *rdcnt_l, *rdcnt_h, *rdcnt_trig;
	struct regmap_field *rst;
};

static int ssd20xd_rtcpwc_isoctrl(struct ssd20xd_rtcpwc *rtcpwc)
{
	static const unsigned int sequence[] = {
		0x1, 0x3, 0x7, 0x5, 0x1, 0x0,
	};
	unsigned int val;
	struct device *dev = &rtcpwc->rtc_dev->dev;
	int i, ret;

	/*
	 * The vendor code has this as part of the sequence
	 * but after testing this doesn't trigger a change in the
	 * ack bit. Instead the vendor code has a loop that breaks
	 * when ack is zero but the ack bit is always zero so it's
	 * pointless. I think the intention here is to reset the
	 * register to zero and it isn't actually part of the
	 * sequence.
	 */
	regmap_field_force_write(rtcpwc->iso_ctrl, 0);

	for(i = 0; i < ARRAY_SIZE(sequence); i++) {
		unsigned int ack;
		/*
		 * The ack bit is seems to be toggled by whatever is actually
		 * handling the operation so we need to know what the original
		 * value was to check if it got inverted.
		 */
		regmap_field_read(rtcpwc->iso_ctrl_ack, &ack);

		printk("original ack %d\n", ack);

		regmap_field_force_write(rtcpwc->iso_ctrl, sequence[i]);
		/*
		 * No idea what the correct values are for this.
		 * Vendor code sleeps for 100us per loop and loops up
		 * to 20 times.
		 */
		ret = regmap_field_read_poll_timeout(rtcpwc->iso_ctrl_ack, val, val != ack, 100, 20 * 100);
		if(ret) {
			printk("loop %d\n", val);
			dev_err(dev, "Timeout waiting for ack byte %i (%x) of sequence\n", i, sequence[i]);
			return ret;
		}
	}

	/*
	 * Same deal as above. No idea of the right values here.
	 * Vendor code loops 22 times with 100us delay.
	 */
	ret = regmap_field_read_poll_timeout(rtcpwc->iso_en, val, val, 100, 22 * 100);
	if(ret){
		dev_err(dev, "Timeout waiting for iso en\n");
	}

	mdelay(20);

	return 0;
}

static int ssd20xd_rtcpwc_read_base(struct ssd20xd_rtcpwc *priv)
{
	unsigned l, h, base;

	regmap_field_write(priv->base_rd, 1);
	ssd20xd_rtcpwc_isoctrl(priv);
	regmap_field_read(priv->rddata_l, &l);
	regmap_field_read(priv->rddata_h, &h);
	regmap_field_write(priv->base_rd, 0);

	base = (h << 16) | l;

	printk("base --- %x\n", base);

	return 0;
}

static int ssd20xd_rtcpwc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct ssd20xd_rtcpwc *priv = dev_get_drvdata(dev);
	unsigned updating, l, h;
	u32 seconds;

	regmap_field_write(priv->rdcnt_trig, 1);
	regmap_field_read_poll_timeout(priv->cnt_updating, updating, !updating, 0, 0);
	regmap_field_read(priv->rdcnt_l, &l);
	regmap_field_read(priv->rdcnt_h, &h);
	regmap_field_write(priv->rdcnt_trig, 0);

	seconds = h << 16 | l;

	rtc_time64_to_tm(seconds, tm);

	return 0;
}

static int ssd20xd_rtcpwc_write_base(struct ssd20xd_rtcpwc* priv)
{
	regmap_field_write(priv->base_wr, 1);
	regmap_field_write(priv->wrdata_l, 0xaba3);
	regmap_field_write(priv->wrdata_h, 0x5b5b);
	ssd20xd_rtcpwc_isoctrl(priv);
	regmap_field_write(priv->base_wr, 0);

	return 0;
}
static int ssd20xd_rtcpwc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct ssd20xd_rtcpwc *priv = dev_get_drvdata(dev);
	unsigned long seconds;

	ssd20xd_rtcpwc_write_base(priv);
	ssd20xd_rtcpwc_read_base(priv);

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

	priv->base_wr = devm_regmap_field_alloc(dev, regmap, base_wr_field);
	if(IS_ERR(priv->base_wr))
		return PTR_ERR(priv->base_wr);

	priv->base_rd = devm_regmap_field_alloc(dev, regmap, base_rd_field);
	if(IS_ERR(priv->base_rd))
		return PTR_ERR(priv->base_rd);

	priv->iso_ctrl = devm_regmap_field_alloc(dev, regmap, iso_ctrl_field);
	if(IS_ERR(priv->iso_ctrl))
		return PTR_ERR(priv->iso_ctrl);

	priv->iso_ctrl_ack = devm_regmap_field_alloc(dev, regmap, iso_ctrl_ack_field);
	if(IS_ERR(priv->iso_ctrl_ack))
		return PTR_ERR(priv->iso_ctrl_ack);

	priv->iso_en = devm_regmap_field_alloc(dev, regmap, iso_en_field);
	if(IS_ERR(priv->iso_en))
		return PTR_ERR(priv->iso_en);

	priv->wrdata_l = devm_regmap_field_alloc(dev, regmap, wrdata_l_field);
	if(IS_ERR(priv->wrdata_l))
		return PTR_ERR(priv->wrdata_l);

	priv->wrdata_h = devm_regmap_field_alloc(dev, regmap, wrdata_h_field);
	if(IS_ERR(priv->wrdata_h))
		return PTR_ERR(priv->wrdata_h);

	priv->rddata_l = devm_regmap_field_alloc(dev, regmap, rddata_l_field);
	if(IS_ERR(priv->rddata_l))
		return PTR_ERR(priv->rddata_l);

	priv->rddata_h = devm_regmap_field_alloc(dev, regmap, rddata_h_field);
	if(IS_ERR(priv->rddata_h))
		return PTR_ERR(priv->rddata_h);

	priv->cnt_updating = devm_regmap_field_alloc(dev, regmap, cnt_updating_field);
	if(IS_ERR(priv->cnt_updating))
		return PTR_ERR(priv->cnt_updating);

	priv->rdcnt_l = devm_regmap_field_alloc(dev, regmap, rddata_cnt_l_field);
	if(IS_ERR(priv->rdcnt_l))
		return PTR_ERR(priv->rdcnt_l);

	priv->rdcnt_h = devm_regmap_field_alloc(dev, regmap, rddata_cnt_h_field);
	if(IS_ERR(priv->rdcnt_h))
		return PTR_ERR(priv->rdcnt_h);

	priv->rdcnt_trig = devm_regmap_field_alloc(dev, regmap, cnt_rd_trig_field);
	if(IS_ERR(priv->rdcnt_trig))
		return PTR_ERR(priv->rdcnt_trig);

	priv->rst = devm_regmap_field_alloc(dev, regmap, rst_field);

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
