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
#include <linux/dmaengine.h>

#define DRIVER_NAME "ssd20xd-movedma"
#define CHANNELS 1

static const struct regmap_config ssd20xd_movedma_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4
};

struct ssd20xd_movedma {
	struct dma_device dma_device;
	struct clk *clk;
};

struct ssd20xd_movedma_chan {
	struct dma_chan chan;
	int irq;
	struct regmap *regmap;
	struct list_head queue;
	dma_cookie_t cookie;
};

#define to_chan(ch) container_of(ch, struct ssd20xd_movedma_chan, chan);

struct ssd20xd_movedma_desc {
	struct dma_async_tx_descriptor tx;
	size_t len;
	dma_addr_t dst;
	dma_addr_t src;
	struct list_head queue_node;
};

#define to_desc(desc) container_of(desc, struct ssd20xd_movedma_desc, tx);

static const struct of_device_id ssd20xd_movedma_of_match[] = {
	{ .compatible = "sstar,ssd20xd-movedma", },
	{},
};

static irqreturn_t ssd20xd_movedma_irq(int irq, void *data)
{
	struct ssd20xd_movedma *movedma = data;
	return IRQ_HANDLED;
}

static enum dma_status ssd20xd_movedma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate){
	printk("movedma tx status\n");
	return DMA_ERROR;
}

void ssd20xd_movedma_issue_pending(struct dma_chan *chan){
	printk("movedma issue pending\n");
}

static dma_cookie_t msc313_tx_submit(struct dma_async_tx_descriptor *tx){
	struct ssd20xd_movedma_chan *chan = to_chan(tx->chan);
	struct ssd20xd_movedma_desc *desc = to_desc(tx);
	list_add_tail(&desc->queue_node, &chan->queue);
	return chan->cookie++;
}

static struct dma_async_tx_descriptor* ssd20xd_movedma_prep_dma_memcpy(
		struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		size_t len, unsigned long flags){

	struct ssd20xd_movedma_desc *desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	dma_async_tx_descriptor_init(&desc->tx, chan);
	desc->len = len;
	desc->src = src;
	desc->dst = dst;
	desc->tx.tx_submit = msc313_tx_submit;

	return &desc->tx;
};

static int ssd20xd_movedma_probe(struct platform_device *pdev)
{
	struct ssd20xd_movedma *movedma;
	struct ssd20xd_movedma_chan *chan;
	struct resource *res;
	void __iomem *base;
	int i, ret;

	printk("movedma probe\n");

	movedma = devm_kzalloc(&pdev->dev, sizeof(*movedma), GFP_KERNEL);
	if (!movedma)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		return PTR_ERR(base);
	}

	//movedma->clk = devm_clk_get(&pdev->dev, NULL);
	//if (IS_ERR(movedma->clk)) {
	//	return PTR_ERR(movedma->clk);
	//}

	movedma->dma_device.dev = &pdev->dev;
	movedma->dma_device.device_tx_status = ssd20xd_movedma_tx_status;
	movedma->dma_device.device_issue_pending = ssd20xd_movedma_issue_pending;
	movedma->dma_device.src_addr_widths = BIT(4);
	movedma->dma_device.dst_addr_widths = BIT(4);
	movedma->dma_device.directions = BIT(DMA_MEM_TO_MEM);
	movedma->dma_device.device_prep_dma_memcpy = ssd20xd_movedma_prep_dma_memcpy;

	INIT_LIST_HEAD(&movedma->dma_device.channels);

	dma_cap_set(DMA_MEMCPY, movedma->dma_device.cap_mask);

	for(i = 0; i < CHANNELS; i++){
		chan = devm_kzalloc(&pdev->dev, sizeof(*chan), GFP_KERNEL);
		if (!chan)
			return -ENOMEM;

		INIT_LIST_HEAD(&chan->queue);

		chan->regmap = devm_regmap_init_mmio(&pdev->dev, base + (0x40 * i),
				&ssd20xd_movedma_regmap_config);
		if(IS_ERR(chan->regmap)){
			return PTR_ERR(chan->regmap);
		}

		chan->irq = irq_of_parse_and_map(pdev->dev.of_node, i);
		if (!chan->irq)
			return -EINVAL;
		ret = devm_request_irq(&pdev->dev, chan->irq, ssd20xd_movedma_irq, IRQF_SHARED,
				dev_name(&pdev->dev), chan);

		chan->chan.device = &movedma->dma_device;

		list_add_tail(&chan->chan.device_node, &movedma->dma_device.channels);
	}

	ret = dma_async_device_register(&movedma->dma_device);
	if(ret)
		goto out;

	ret = clk_prepare_enable(movedma->clk);
	if (ret)
		goto out;

	out:
	return ret;
}

static struct platform_driver ssd20xd_movedma_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ssd20xd_movedma_of_match,
	},
	.probe = ssd20xd_movedma_probe,
};
builtin_platform_driver(ssd20xd_movedma_driver);
