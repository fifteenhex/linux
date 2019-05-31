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
#include <linux/of_dma.h>
#include "../dmaengine.h"

/*
 * MStar BDMA controller - B seems to be for byte
 *
 * This is a simple dma controller for transferring blocks from
 * a to b. The only special thing about it seems to be that it
 * can also do CRC32 calculations.
 *
 *
 * 0x0 - ctrl
 * vendor sdk says "set source and destination path" for this
 *
 * 9       | 8        | 7 - 5 | 4    | 3 - 1 | 0
 * dst_tlb | src_tlb? |       | stop |       | trigger
 *
 * writing 0xffff results in 0x310
 *
 * writing 0x11 allows writing 0 to clear trigger
 *
 * 0x4 - status
 * 14 | 7 | 6 | 5 | 4            |  3   |  2  |  1   |   0    |
 *  ? | ? | ? |   | result0(err) | done | int | busy | queued |
 *
 * 2 - 4 -- seem to status flags
 * 0 - 1 -- seem to busy indicators
 *
 * 0x8  - src, dst, width
 *      14 - 12   |   11 - 8  |     6 - 4      | 3 - 0
 *    dst width   |    dst    |   src width    |  src
 *
 * src/dst ids for infinity3
 *
 *      | src                 | dst         |
 * 0x0  | MIU - 16 bytes wide | same as src |
 * 0x1  | IMI?                |             |
 * 0x4  | Fill? - 4           |             |
 * 0x5  | QSPI - 8 bytes wide |             |
 * 0xB  |                     | FSP?        |
 *
 * width
 * 0x0 - 1 byte
 * 0x1 - 2 bytes
 * 0x2 - 4 bytes
 * 0x3 - 8 bytes
 * 0x4 - 16 bytes
 *
 * 0xc - misc
 * 16 - 5 |    1    | 0
 *        | int_en  | direction
 *    ?   |         | 0 - increment address
 *        |         | 1 - decrement address
 *
 * 4 - 7  - probably configuration for the CRC32 mode
 * 8 - 11 - might be dmywrcnt
 *
 * 0x10 - start address low
 * 0x14 - start address high
 * 0x18 - end address low
 * 0x1c - end address high
 * 0x20 - size low
 * 0x24 - size high
 *
 * -- spotted in another mstar bdma driver, might not exist but are writable--
 * 0x28 - cmd
 * 0x2c - " "
 * 0x30 - " "
 * 0x34 - " "
 *
 * -- writable
 *
 * 0x38
 * 0x3c
 */

#define DRIVER_NAME "msc313-bdma"
#define CHANNELS 2

#define REG_CTRL		0x0
#define REG_STATUS		0x4
#define STATUS_ERR		BIT(4)
#define STATUS_DONE		BIT(3)
#define STATUS_INT		BIT(2)
#define REG_CONFIG		0x8
#define REG_MISC		0xc
#define REG_SRC_ADDR_L		0x10
#define REG_DST_ADDR_L		0x18
#define REG_SIZE_L		0x20

#define CHANNEL_SIZE		0x40

static struct reg_field ctrl_trigger = REG_FIELD(REG_CTRL, 0, 0);
static struct reg_field ctrl_stop = REG_FIELD(REG_CTRL, 4, 4);
static struct reg_field config_src = REG_FIELD(REG_CONFIG, 0, 3);
static struct reg_field config_src_width = REG_FIELD(REG_CONFIG, 4, 6);
static struct reg_field misc_int_en = REG_FIELD(REG_MISC, 1, 1);
static struct reg_field config_dst = REG_FIELD(REG_CONFIG, 8, 11);
static struct reg_field config_dst_width = REG_FIELD(REG_CONFIG, 12, 14);

#define WIDTH_8			0x3
#define WIDTH_16		0x4
#define SLAVE_ID_MIU		0
#define SLAVE_WIDTH_MIU		WIDTH_16

struct msc313_bdma_chan {
	struct dma_chan chan;
	int irq;
	struct regmap *regmap;

	spinlock_t lock;
	struct tasklet_struct tasklet;

	struct list_head queue;
	struct list_head completed;

	u8 slave_id;
	enum dma_slave_buswidth slave_width;
	u32 slave_offset;

	char name[8];
	struct regmap_field *int_en;
	struct regmap_field *trigger;
	struct regmap_field *stop;
	struct regmap_field *src;
	struct regmap_field *src_width;
	struct regmap_field *dst;
	struct regmap_field *dst_width;

	struct msc313_bdma_desc* inflight;
};

struct msc313_bdma {
	struct dma_device dma_device;
	struct clk *clk;
	unsigned numchans;
	struct msc313_bdma_chan *chans;
};

#define to_chan(ch) container_of(ch, struct msc313_bdma_chan, chan);

struct msc313_bdma_desc {
	struct dma_async_tx_descriptor tx;
	size_t len;
	dma_addr_t dst;
	dma_addr_t src;
	u8 src_id;
	u8 dst_id;
	u8 src_width;
	u8 dst_width;
	bool success;
	struct list_head queue_node;
};

#define to_desc(desc) container_of(desc, struct msc313_bdma_desc, tx);

static const struct of_device_id msc313_bdma_of_match[] = {
	{ .compatible = "mstar,msc313-bdma", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_bdma_of_match);

static void msc313_bdma_tasklet(unsigned long data){
	struct msc313_bdma_chan *chan = (struct msc313_bdma_chan*) data;
	struct msc313_bdma_desc *desc;
	unsigned long flags;
	struct list_head *cur, *tmp;

	spin_lock_irqsave(&chan->lock, flags);

	list_for_each_safe(cur, tmp, &chan->completed) {
		desc = list_entry(cur, struct msc313_bdma_desc, queue_node);
		list_del(&desc->queue_node);
		dma_cookie_complete(&desc->tx);
		dma_descriptor_unmap(&desc->tx);
		dmaengine_desc_get_callback_invoke(&desc->tx, NULL);
		dma_run_dependencies(&desc->tx);
		this_cpu_ptr(chan->chan.local)->bytes_transferred += desc->len;
		kfree(desc);
	}
	spin_unlock_irqrestore(&chan->lock, flags);
}

static irqreturn_t msc313_bdma_irq(int irq, void *data)
{
	struct msc313_bdma_chan *chan = data;
	unsigned int status;
	unsigned long flags;
	regmap_read(chan->regmap, REG_STATUS, &status);
	regmap_write(chan->regmap, REG_STATUS, STATUS_ERR | STATUS_DONE | STATUS_INT);
	//printk("bdma int %04x\n", status);
	chan->inflight->success = true;

	spin_lock_irqsave(&chan->lock, flags);
	list_move_tail(&chan->inflight->queue_node, &chan->completed);
	spin_unlock_irqrestore(&chan->lock, flags);

	chan->inflight = NULL;
	tasklet_schedule(&chan->tasklet);
	return IRQ_HANDLED;
}

static enum dma_status msc313_bdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate){
	enum dma_status ret;
	ret = dma_cookie_status(chan, cookie, txstate);
	return ret;
}

static void msc313_bdma_write_low_high_pair(struct msc313_bdma_chan *chan, u8 reg, u32 value){
	regmap_write(chan->regmap, reg, value);
	regmap_write(chan->regmap, reg + 4, value >> 16);
}

static void msc313_bdma_do_single(struct msc313_bdma_chan *chan, struct msc313_bdma_desc* desc){
	//unsigned int status;
	chan->inflight = desc;

	regmap_field_write(chan->stop, 1);
	regmap_field_write(chan->stop, 0);

	regmap_field_write(chan->src, desc->src_id);
	regmap_field_write(chan->src_width, desc->src_width);
	regmap_field_write(chan->dst, desc->dst_id);
	regmap_field_write(chan->dst_width, desc->dst_width);

	msc313_bdma_write_low_high_pair(chan, REG_SRC_ADDR_L, desc->src);
	msc313_bdma_write_low_high_pair(chan, REG_DST_ADDR_L, desc->dst);
	msc313_bdma_write_low_high_pair(chan, REG_SIZE_L, desc->len);

	//regmap_read(chan->regmap, REG_STATUS, &status);
	//printk("s before t %04x\n", status);
	regmap_field_force_write(chan->trigger, 1);
	//regmap_read(chan->regmap, REG_STATUS, &status);
	//printk("s after t %04x\n", status);
}

void msc313_bdma_issue_pending(struct dma_chan *chan){
	struct msc313_bdma_desc *desc;
	struct msc313_bdma_chan *ch = to_chan(chan);
	desc = list_first_entry_or_null(&ch->queue, struct msc313_bdma_desc, queue_node);
	if(desc)
		msc313_bdma_do_single(ch, desc);
}

static dma_cookie_t msc313_tx_submit(struct dma_async_tx_descriptor *tx){
	dma_cookie_t cookie;
	struct msc313_bdma_desc *desc;
	struct msc313_bdma_chan *chan;

	chan = to_chan(tx->chan);
	desc = to_desc(tx);

	list_add_tail(&desc->queue_node, &chan->queue);
	cookie = dma_cookie_assign(tx);

	return cookie;
}

static struct dma_async_tx_descriptor* msc313_bdma_prep_dma_memcpy(
		struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		size_t len, unsigned long flags){

	struct msc313_bdma_desc *desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	dma_async_tx_descriptor_init(&desc->tx, chan);

	desc->len = len;
	desc->src_id = SLAVE_ID_MIU;
	desc->src = src - 0x20000000;
	desc->src_width = SLAVE_WIDTH_MIU;
	desc->dst_id = SLAVE_ID_MIU;
	desc->dst = dst - 0x20000000;
	desc->dst_width = SLAVE_WIDTH_MIU;

	//printk("memcpy %08x %08x\n", desc->dst, desc->src);

	desc->tx.tx_submit = msc313_tx_submit;

	return &desc->tx;
};

static struct dma_async_tx_descriptor* msc313_bdma_prep_slave_sg(
		struct dma_chan *chan, struct scatterlist *sgl,
		unsigned int sg_len, enum dma_transfer_direction direction,
		unsigned long flags, void *context)
{
	struct msc313_bdma_chan *ch;
	struct msc313_bdma_desc *desc;
	u32 dmaaddr, dmalen;

	if(sg_len != 1){
		printk("only one sg for now\n");
		return NULL;
	}

	ch = to_chan(chan);

	dmaaddr = sg_dma_address(sgl);
	dmalen = sg_dma_len(sgl);

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	dma_async_tx_descriptor_init(&desc->tx, chan);

	desc->len = dmalen;

	switch(direction){
		case DMA_DEV_TO_MEM:
			desc->dst_id = SLAVE_ID_MIU;
			desc->dst = dmaaddr - 0x20000000;
			desc->dst_width = SLAVE_WIDTH_MIU;
			desc->src_id = ch->slave_id;
			desc->src = ch->slave_offset;
			switch (ch->slave_width) {
			case DMA_SLAVE_BUSWIDTH_8_BYTES:
				desc->src_width = WIDTH_8;
				break;
			case DMA_SLAVE_BUSWIDTH_16_BYTES:
				desc->src_width = WIDTH_16;
				break;
			default:
				goto free_desc;
			}
			break;
		case DMA_MEM_TO_DEV:
			desc->src_id = SLAVE_ID_MIU;
			desc->src = dmaaddr - 0x20000000;
			desc->src_width = SLAVE_WIDTH_MIU;
			desc->dst_id = ch->slave_id;
			desc->dst = ch->slave_offset;
			switch (ch->slave_width) {
			case DMA_SLAVE_BUSWIDTH_8_BYTES:
				desc->dst_width = WIDTH_8;
				break;
			case DMA_SLAVE_BUSWIDTH_16_BYTES:
				desc->dst_width = WIDTH_16;
				break;
			default:
				goto free_desc;
			}
			break;
		default:
			goto free_desc;
	}

	desc->tx.tx_submit = msc313_tx_submit;

	//printk("sg %d:%08x -> %d:%08x\n", (int) desc->src_id, desc->src, (int) desc->dst_id, desc->dst);

	return &desc->tx;

free_desc:
	kfree(desc);
	return NULL;
}

static int msc313_bdma_config(struct dma_chan *chan,
		struct dma_slave_config *config){
	struct msc313_bdma_chan *ch = to_chan(chan);
	ch->slave_id = config->slave_id;
	ch->slave_width = config->src_addr_width;
	ch->slave_offset = config->src_addr;
	return 0;
}

static int msc313_bdma_probe(struct platform_device *pdev)
{
	struct msc313_bdma *bdma;
	struct msc313_bdma_chan *chan;
	struct resource *res;
	void __iomem *base;
	int i, ret;
	struct regmap_config regmap_config = {
			.reg_bits = 16,
			.val_bits = 16,
			.reg_stride = 4,
	};

	bdma = devm_kzalloc(&pdev->dev, sizeof(*bdma), GFP_KERNEL);
	if (!bdma)
		return -ENOMEM;

	platform_set_drvdata(pdev, bdma);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		return PTR_ERR(base);
	}

	bdma->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(bdma->clk)) {
		return PTR_ERR(bdma->clk);
	}

	bdma->dma_device.dev = &pdev->dev;
	bdma->dma_device.device_tx_status = msc313_bdma_tx_status;
	bdma->dma_device.device_issue_pending = msc313_bdma_issue_pending;
	bdma->dma_device.src_addr_widths = BIT(4);
	bdma->dma_device.dst_addr_widths = BIT(4);
	bdma->dma_device.directions = BIT(DMA_MEM_TO_MEM) | BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	bdma->dma_device.device_prep_dma_memcpy = msc313_bdma_prep_dma_memcpy;
	bdma->dma_device.device_prep_slave_sg = msc313_bdma_prep_slave_sg;
	bdma->dma_device.device_config = msc313_bdma_config;
	bdma->dma_device.copy_align = 2;

	INIT_LIST_HEAD(&bdma->dma_device.channels);

	dma_cap_set(DMA_MEMCPY, bdma->dma_device.cap_mask);

	bdma->numchans = CHANNELS;

	bdma->chans = devm_kzalloc(&pdev->dev, sizeof(*chan) * bdma->numchans, GFP_KERNEL);
	if (!bdma->chans)
		return -ENOMEM;

	for(i = 0; i < bdma->numchans; i++){
		chan = &bdma->chans[i];
		snprintf(chan->name, sizeof(chan->name),"ch%d", i);
		regmap_config.name = chan->name;

		INIT_LIST_HEAD(&chan->queue);
		INIT_LIST_HEAD(&chan->completed);

		chan->regmap = devm_regmap_init_mmio(&pdev->dev, base + (CHANNEL_SIZE * i),
				&regmap_config);
		if(IS_ERR(chan->regmap)){
			return PTR_ERR(chan->regmap);
		}

		chan->trigger = devm_regmap_field_alloc(&pdev->dev, chan->regmap, ctrl_trigger);
		chan->stop = devm_regmap_field_alloc(&pdev->dev, chan->regmap, ctrl_stop);
		chan->int_en = devm_regmap_field_alloc(&pdev->dev, chan->regmap, misc_int_en);
		chan->src = devm_regmap_field_alloc(&pdev->dev, chan->regmap, config_src);
		chan->src_width = devm_regmap_field_alloc(&pdev->dev, chan->regmap, config_src_width);
		chan->dst = devm_regmap_field_alloc(&pdev->dev, chan->regmap, config_dst);
		chan->dst_width = devm_regmap_field_alloc(&pdev->dev, chan->regmap, config_dst_width);

		// this should be moved until after everything is registered
		regmap_field_write(chan->int_en, 1);

		chan->irq = irq_of_parse_and_map(pdev->dev.of_node, i);
		if (!chan->irq)
			return -EINVAL;
		ret = devm_request_irq(&pdev->dev, chan->irq, msc313_bdma_irq, IRQF_SHARED,
				dev_name(&pdev->dev), chan);

		chan->chan.device = &bdma->dma_device;

		tasklet_init(&chan->tasklet, msc313_bdma_tasklet, (unsigned long) chan);

		dma_cookie_init(&chan->chan);

		list_add_tail(&chan->chan.device_node, &bdma->dma_device.channels);
	}

	ret = dma_async_device_register(&bdma->dma_device);
	if(ret)
		goto out;

	ret = of_dma_controller_register(pdev->dev.of_node,
			of_dma_xlate_by_chan_id, &bdma->dma_device);

	ret = clk_prepare_enable(bdma->clk);
	if (ret)
		goto out;

	out:
	return ret;
}

static int msc313_bdma_remove(struct platform_device *pdev)
{
	return 0;
}

static int __maybe_unused msc313_bdma_suspend(struct device *dev)
{
	struct msc313_bdma *bdma = dev_get_drvdata(dev);
	int i;
	for(i = 0; i < bdma->numchans; i++)
		regmap_field_write(bdma->chans[i].int_en, 0);
	return 0;
}

static int __maybe_unused msc313_bdma_resume(struct device *dev)
{
	struct msc313_bdma *bdma = dev_get_drvdata(dev);
	int i;
	for(i = 0; i < bdma->numchans; i++)
			regmap_field_write(bdma->chans[i].int_en, 1);
	return 0;
}

static SIMPLE_DEV_PM_OPS(msc313_bdma_pm_ops, msc313_bdma_suspend,
			 msc313_bdma_resume);

static struct platform_driver msc313_bdma_driver = {
	.probe = msc313_bdma_probe,
	.remove = msc313_bdma_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_bdma_of_match,
		.pm = &msc313_bdma_pm_ops,
	},
};

module_platform_driver(msc313_bdma_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 BDMA driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
