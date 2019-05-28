// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/pwm.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/i2c.h>
#include <linux/delay.h>

/*
 * MSC313 i2c controller
 *
 * 0x00 : ctrl
 *   2   |   1   |   0
 * enint | endma |  rst
 *
 * 0x04 : start/stop
 *   8  |   0
 * stop | start
 *
 * 0x08 : write data
 *  8	| 7 - 0
 * ack  | data
 *
 * 0x0c : read data
 *   9  |  8   | 7 - 0
 *  ack | trig | data
 *
 * ack and trig have to be written together to trigger a read with the correct
 * ack/nack at the end
 *
 * 0x10 : int ctl
 *
 * 0x14 : status/int status
 *   13   |   12   |   11   |   10   |    9    |    8     |  4 - 0
 * sclerr | clkstr | txdone | rxdone | stopdet | startdet | hw state
 *                                                        | 0 - idle
 *                                                        | 1 or 2 - start
 *                                                        | 3,4,5 or 6 - write
 *                                                        | 7,8,9,10 - read
 *                                                        | 11 - interrupt
 *                                                        | 12 - wait
 *                                                        | 13,14,15 - stop
 *
 * The below registers are apparently for the number of clocks per state
 * 0x20: stp_cnt (stop clock count?)
 * 0x24: ckh_cnt (clock high count?)
 * 0x28: ckl_cnt (clock low count?)
 * 0x2c: sda_cnt
 * 0x30: stt_cnt
 * 0x34: lth_cnt
 * 0x38: tmt_cnt
 *
 * 0x3c: scli delay?
 *
 * todo: each controller has it's own DMA controller
 */

#define DRIVER_NAME "msc313e-i2c"

#define REG_CTRL	0x00
#define REG_STARTSTOP	0x04
#define REG_WDATA	0x08
#define REG_RDATA	0x0c
#define REG_INT_CTRL 	0x10
#define REG_INT_STAT	0x14
#define REG_CKH_CNT	0x24
#define REG_CKL_CNT	0x28

struct msc313e_i2c {
	struct i2c_adapter i2c;
	struct clk_hw sclk;
	struct regmap *regmap;
	struct regmap_field* rst;
	struct regmap_field* enint;
	struct regmap_field* start;

	/* rx data fields */
	struct regmap_field* rdata;
	struct regmap_field* rtrigack;

	/* tx data fields */
	struct regmap_field* wdata;
	struct regmap_field* ack;

	struct regmap_field* stop;
	struct regmap_field* state;
	struct regmap_field* intstat;

	/* timing */
	struct regmap_field* clkhcount;
	struct regmap_field* clklcount;

	wait_queue_head_t wait;
	bool done;
};

static const struct regmap_config msc313e_i2c_regmap_config = {
		.name = DRIVER_NAME,
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4
};

static const struct reg_field ctrl_rst_field = REG_FIELD(REG_CTRL, 0, 0);
static const struct reg_field ctrl_enint_field = REG_FIELD(REG_CTRL, 2, 2);
static const struct reg_field startstop_start_field = REG_FIELD(REG_STARTSTOP, 0, 0);
static const struct reg_field startstop_stop_field = REG_FIELD(REG_STARTSTOP, 8, 8);

static const struct reg_field wdata_data_field = REG_FIELD(REG_WDATA, 0, 7);
static const struct reg_field wdata_ack_field = REG_FIELD(REG_WDATA, 8, 8);

static const struct reg_field rdata_data_field = REG_FIELD(REG_RDATA, 0, 7);
static const struct reg_field rdata_trigack_field = REG_FIELD(REG_RDATA, 8, 9);

static const struct reg_field status_state_field = REG_FIELD(REG_INT_STAT, 0, 4);
static const struct reg_field status_int_field = REG_FIELD(REG_INT_STAT, 8, 13);

static const struct reg_field ckhcnt_field = REG_FIELD(REG_CKH_CNT, 0, 15);
static const struct reg_field cklcnt_field = REG_FIELD(REG_CKL_CNT, 0, 15);

static irqreturn_t msc313_i2c_irq(int irq, void *data)
{
	struct msc313e_i2c *i2c = data;
	unsigned int intstatus;

	regmap_field_read(i2c->intstat, &intstatus);
	regmap_write(i2c->regmap, REG_INT_CTRL, 1);
	i2c->done = true;
	wake_up(&i2c->wait);

	return IRQ_HANDLED;
}

static void printhwstate(struct msc313e_i2c *i2c)
{
	unsigned int state;

	regmap_field_read(i2c->state, &state);
	dev_info(&i2c->i2c.dev, "state %d", (int) state);
}

static int spin(struct msc313e_i2c *i2c)
{
	wait_event_timeout(i2c->wait, i2c->done, HZ / 100);

	if(!i2c->done){
		dev_err(&i2c->i2c.dev, "timeout waiting for hardware to become idle\n");
		printhwstate(i2c);
		return 1;
	}

	udelay(10);

	return 0;
}

static int msc313_i2c_rxbyte(struct msc313e_i2c *i2c, bool last)
{
	unsigned int data;

	i2c->done = false;
	regmap_field_force_write(i2c->rtrigack, (last ? BIT(1) : 0) | BIT(0));
	if(spin(i2c))
		goto err;

	regmap_field_read(i2c->rdata, &data);
	return data;

err:
	return -1;
}

static int msc313_i2c_txbyte(struct msc313e_i2c *i2c, u8 byte)
{
	unsigned int ack;

	i2c->done = false;
	regmap_field_force_write(i2c->wdata, byte);
	spin(i2c);
	regmap_field_read(i2c->ack, &ack);

	return ack;
}

static int msc313_i2c_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg msgs[],
			    int num)
{
	int i,j, ret = 0;
	struct msc313e_i2c *i2c = i2c_get_adapdata(i2c_adap);

	regmap_field_force_write(i2c->rst, 0);
	mdelay(10);

	//printhwstate(i2c);

	for(i = 0; i < num; i++) {
		i2c->done = false;
		regmap_field_force_write(i2c->start, 1);
		spin(i2c);

		if (msc313_i2c_txbyte(i2c, (msgs[i].addr << 1) | (msgs[i].flags & I2C_M_RD)))
			goto nextmsg;

		if (msgs[i].flags & I2C_M_RD) {
			for (j = 0; j < msgs[i].len; j++){
				*(msgs[i].buf + j) = msc313_i2c_rxbyte(i2c, j + 1 == msgs[i].len);
			}
		} else {
			for (j = 0; j < msgs[i].len; j++) {
				if (msc313_i2c_txbyte(i2c, *(msgs[i].buf + j)))
					goto nextmsg;
			}
		}

		ret++;

		nextmsg:
		i2c->done = false;
		regmap_field_force_write(i2c->stop, 1);
		spin(i2c);
	}

	regmap_field_force_write(i2c->rst, 1);
	mdelay(10);

	return ret;
}

static u32 msc313_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm msc313_i2c_algo = {
	.master_xfer = msc313_i2c_xfer,
	.functionality = msc313_i2c_func,
};

static unsigned long msc313e_i2c_sclk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct msc313e_i2c *i2c = container_of(hw, struct msc313e_i2c, sclk);
	unsigned count_high, count_low;

	regmap_field_read(i2c->clkhcount, &count_high);
	regmap_field_read(i2c->clklcount, &count_low);

	return parent_rate / (count_high + count_low);
}

static int msc313_sclk_determine_rate(struct clk_hw *hw,
		struct clk_rate_request *req)
{
	struct msc313e_i2c *i2c = container_of(hw, struct msc313e_i2c, sclk);

	return 0;
}

static const struct clk_ops sclk_ops = {
	.recalc_rate = msc313e_i2c_sclk_recalc_rate,
	.determine_rate = msc313_sclk_determine_rate,
};

static const struct clk_parent_data sclk_parent = {
	.index	= 0,
};

static int msc313e_i2c_probe(struct platform_device *pdev)
{
	struct clk_init_data sclk_init = {
		.ops = &sclk_ops,
		.parent_data = &sclk_parent,
		.num_parents = 1,
	};
	struct device *dev = &pdev->dev;
	struct msc313e_i2c *msc313ei2c;
	struct i2c_adapter *adap;
	void* __iomem base;
	int irq, ret;

	msc313ei2c = devm_kzalloc(&pdev->dev, sizeof(*msc313ei2c), GFP_KERNEL);
	if (!msc313ei2c)
		return -ENOMEM;

	init_waitqueue_head(&msc313ei2c->wait);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	msc313ei2c->regmap = devm_regmap_init_mmio(&pdev->dev, base,
			&msc313e_i2c_regmap_config);
	if(IS_ERR(msc313ei2c->regmap))
		return PTR_ERR(msc313ei2c->regmap);

	msc313ei2c->rst = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, ctrl_rst_field);
	msc313ei2c->enint = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, ctrl_enint_field);
	msc313ei2c->start = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, startstop_start_field);
	msc313ei2c->stop = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, startstop_stop_field);

	msc313ei2c->rdata = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, rdata_data_field);
	msc313ei2c->rtrigack = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, rdata_trigack_field);

	msc313ei2c->ack = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, wdata_ack_field);
	msc313ei2c->wdata = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, wdata_data_field);

	msc313ei2c->state = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, status_state_field);
	msc313ei2c->intstat = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, status_int_field);

	msc313ei2c->clkhcount = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, ckhcnt_field);
	msc313ei2c->clklcount = devm_regmap_field_alloc(&pdev->dev, msc313ei2c->regmap, cklcnt_field);

	regmap_field_write(msc313ei2c->enint, 1);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_i2c_irq, IRQF_SHARED,
			dev_name(&pdev->dev), msc313ei2c);

	sclk_init.name = devm_kasprintf(dev, GFP_KERNEL, "%s_sclk", dev_name(dev));
	msc313ei2c->sclk.init = &sclk_init;
	ret = devm_clk_hw_register(dev, &msc313ei2c->sclk);
	if (ret)
		return ret;

	adap = &msc313ei2c->i2c;
	i2c_set_adapdata(adap, msc313ei2c);
	snprintf(adap->name, sizeof(adap->name), dev_name(&pdev->dev));
	adap->owner = THIS_MODULE;
	adap->timeout = 2 * HZ;
	adap->retries = 0;
	adap->algo = &msc313_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;
	ret = i2c_add_adapter(adap);

	platform_set_drvdata(pdev, msc313ei2c);

	return ret;
}

static int msc313e_i2c_remove(struct platform_device *pdev)
{
	struct msc313e_i2c *msc313ei2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&msc313ei2c->i2c);

	return 0;
}

static const struct of_device_id msc313e_i2c_dt_ids[] = {
	{ .compatible = "mstar,msc313e-i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, msc313e_i2c_dt_ids);

static struct platform_driver msc313e_i2c_driver = {
	.probe = msc313e_i2c_probe,
	.remove = msc313e_i2c_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = msc313e_i2c_dt_ids,
	},
};
module_platform_driver(msc313e_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mstar MSC313E i2c driver");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
