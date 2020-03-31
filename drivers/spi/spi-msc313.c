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
#include <linux/clk-provider.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

/*
 * 0x1f222100
 * 0x1f222300
 * 0x00: write buffer
 * 0x10: read buffer
 * 0x20: size
 * 	11 - 8 | 3 - 0
 * 	 rdsz  | wrsz
 * 0x24: ctrl
 *          | 15 - 8 |  7   |  6   |  2  |  1  | 0
 *          | clkdiv | cpol | cpha | int | rst | en
 * 0x28: dc
 * 0x2c: dc
 * 0x30: frame config
 * 0x38: frame config
 * 0x40: lsb first
 * 0x68: trigger
 * 	   0
 * 	trigger
 * 0x6c: done - transfer done
 *     0
 *   done
 * 0x70: done clear - write to clear done
 *     0
 *   done clear
 * 0x7c: chip select
 * clearing bit enables chip select
 *
 * 0xe0 - ef -- Full duplex read buffer?
 *
 * The vendor driver has defines for these locations
 * calling them "FULL_DEPLUX_RD<n>". There are apparently
 * 16 of them and they each hold 2 bytes of the data that was
 * read in so you can have a total of 32 bytes. But you can only
 * write 8 bytes at a time and the pointer for this buffer seems
 * to reset to 0 when a write happens. TL;DR; not 100% sure how
 * it works.
 */

#define DRIVER_NAME "spi_msc313"

#define REG_WRITEBUF	0x0
#define REG_READBUF	0x10
#define REG_SIZE	0x20
#define REG_CTRL	0x24
#define CTRL_ENABLE	BIT(0)
#define CTRL_RESET	BIT(1)
#define CTRL_INT	BIT(2)
#define REG_TRIGGER	0x68
#define REG_DONE	0x6c
#define REG_DONECLR	0x70
#define REG_CS		0x7c
/* DMA registers might only exist on SSD20XD and later */
#define REG_DMALEN_L	0xc0
#define REG_DMALEN_H	0xc4
#define REG_DMAEN	0xc8
#define REG_DMARW	0xcc
#define REG_FDREADBUF	0xE0

#define DIV_SHIFT	8
#define DIV_WIDTH	3

#define FIFOSZ		8

static struct reg_field size_write_field = REG_FIELD(REG_SIZE, 0, 3);
static struct reg_field size_read_field = REG_FIELD(REG_SIZE, 8, 11);
static struct reg_field ctrl_cpha_field = REG_FIELD(REG_CTRL, 6, 6);
static struct reg_field ctrl_cpol_field = REG_FIELD(REG_CTRL, 7, 7);
static struct reg_field trigger_field = REG_FIELD(REG_TRIGGER, 0, 0);
static struct reg_field done_done_field = REG_FIELD(REG_DONE, 0, 0);
static struct reg_field doneclr_field = REG_FIELD(REG_DONECLR, 0, 0);
static struct reg_field cs_field = REG_FIELD(REG_CS, 0, 0);
static struct reg_field dmaen_field = REG_FIELD(REG_DMAEN, 0, 0);
static struct reg_field dmarw_field = REG_FIELD(REG_DMARW, 0, 0);

struct msc313_spi {
	struct device *dev;
	struct spi_master *master;
	struct clk_hw *divider;
	int irq;
	struct regmap *regmap;
	struct regmap_field *cpol;
	struct regmap_field *cpha;
	struct regmap_field *wrsz;
	struct regmap_field *rdsz;
	struct regmap_field *done;
	struct regmap_field *doneclr;

	struct regmap_field *trigger;
	struct regmap_field *cs;

	/* dma */
	struct regmap_field *dmaen;
	struct regmap_field *dmarw;

	wait_queue_head_t wait;

	bool tfrdone;

	spinlock_t lock;

	struct dma_chan *dmachan;
	wait_queue_head_t dma_wait;
	bool dma_done;
	bool dma_success;
};

static const struct regmap_config msc313_spi_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int msc313_spi_setup(struct spi_device *spi)
{
	struct msc313_spi *mspi = spi_master_get_devdata(spi->master);

	regmap_field_write(mspi->cpha, spi->mode & SPI_CPHA ? 1 : 0);
	regmap_field_write(mspi->cpol, spi->mode & SPI_CPOL ? 1 : 0);

	return 0;
}

static void msc313_spi_loadtxbuff(struct msc313_spi *mspi, const u8 *txbuff, unsigned int len)
{
	int i, j, b, reg;
	unsigned int value;

	for (i = 0; (i < FIFOSZ/2) && (i * 2 < len); i++) {
		value = 0;
		b = i * 2;
		for (j = 0; j < 2 && (b + j < len); j++) {
			value |= (unsigned int)(*txbuff++) << (8 * j);
		}
		reg = REG_WRITEBUF + (i * 0x4);
		regmap_write(mspi->regmap, reg, value);
		//printk("wrote %d -> %x\n", i, value);
	}
}

static void msc313_spi_saverxbuff(struct msc313_spi *mspi, unsigned int bufoff,
		u8 *rxbuff, unsigned int len)
{
	int i, j, reg;
	unsigned int value;

	for (i = 0; (i < FIFOSZ/2) && (i * 2 < len); i++) {
		reg = bufoff + (i * 0x4);
		regmap_read(mspi->regmap, reg, &value);
		//printk("read %d -> %x\n", i, value);
		for (j = i * 2; (j < ((i * 2) + 2)) && (j < len); j++) {
			*rxbuff++ = value & 0xff;
			value = value >> 8;
		}
	}
}

static void msc313_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct msc313_spi *mspi = spi_master_get_devdata(spi->master);

	regmap_field_write(mspi->cs, enable ? 1 : 0);
}

static void msc313_spi_dma_callback(void *dma_async_param,
				    const struct dmaengine_result *result)
{

}

static int msc313_spi_transfer_one_dma(struct spi_controller *ctlr, struct spi_device *spi,
		    struct spi_transfer *transfer)
{
	struct msc313_spi *mspi = spi_controller_get_devdata(ctlr);
	struct dma_async_tx_descriptor *dmadesc;
	const u8 *txbuf = transfer->tx_buf;
	unsigned int len = transfer->len;
	struct dma_slave_config config;
	u8 *rxbuf = transfer->rx_buf;
	dma_addr_t dmaaddr = 0;
	enum dma_data_direction dmadir = rxbuf ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	enum dma_transfer_direction dmatrans = rxbuf ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;
	int ret = 0;

	printk("%s:%d\n", __func__, __LINE__);

	regmap_field_write(mspi->dmaen, 1);

	dmaaddr = dma_map_single(mspi->dev, txbuf, len, dmadir);
	ret = dma_mapping_error(mspi->dev, dmaaddr);
	if (ret)
		return ret;

	printk("%s:%d\n", __func__, __LINE__);

	dmadesc = dmaengine_prep_slave_single(mspi->dmachan, dmaaddr, len, dmatrans, 0);
	if (!dmadesc) {
		ret = -ENOMEM;
		goto out;
	}
	dmadesc->callback_result = msc313_spi_dma_callback;
	dmadesc->callback_param = mspi;

	printk("%s:%d\n", __func__, __LINE__);

	config.direction = dmatrans;
	if (rxbuf)
		config.dst_addr = dmaaddr;
	else
		config.src_addr = dmaaddr;
	dmaengine_slave_config(mspi->dmachan, &config);

	dmaengine_submit(dmadesc);
	dma_async_issue_pending(mspi->dmachan);

	mspi->dma_done = false;
	if (!wait_event_timeout(mspi->dma_wait, mspi->dma_done, HZ * 10)) {
		dev_err(mspi->dev, "timeout waiting for dma\n");
		ret = -EIO;
		goto out;
	}
#if 0
	rmb();
	if (isp->dma_success)
		goto dma_exit;
	else
		dev_warn(&isp->master->dev, "dma failed, falling back to cpu read\n");
#endif


out:
	dma_unmap_single(mspi->dev, dmaaddr, len, dmadir);

	return ret;
}

static int msc313_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
			    struct spi_transfer *transfer)
{
	struct msc313_spi *mspi = spi_controller_get_devdata(ctlr);
	const u8 *txbuf = transfer->tx_buf;
	unsigned int len = transfer->len;
	u8 *rxbuf = transfer->rx_buf;
	int blksz, txed = 0;
	unsigned int rdbuf;

	clk_set_rate(mspi->divider->clk, transfer->speed_hz);

	/* DMA only works for half duplex */
	if (mspi->dmachan && ((txbuf && !rxbuf) || (rxbuf && !txbuf)))
		return msc313_spi_transfer_one_dma(ctlr, spi, transfer);

	regmap_field_write(mspi->dmaen, 0);

	while (len - txed > 0) {
		blksz = min(len - txed, (unsigned int) FIFOSZ);
		if (txbuf) {
			msc313_spi_loadtxbuff(mspi, txbuf, blksz);
			txbuf += blksz;
			regmap_field_write(mspi->wrsz, blksz);
			regmap_field_write(mspi->rdsz, 0);
			/*
			 * If there was a txbuf then we did a write and the
			 * read back data will be in the other read buffer.
			 */
			rdbuf = REG_FDREADBUF;
		} else if (rxbuf) {
			regmap_field_write(mspi->wrsz, 0);
			regmap_field_write(mspi->rdsz, blksz);
			rdbuf = REG_READBUF;
		} else
			return -EINVAL;

		/* start the transfer */
		mspi->tfrdone = false;
		regmap_field_force_write(mspi->trigger, 1);
		//for(int reg = 0; reg < 0x100; reg += 4) {
		//	unsigned int val;
		//	regmap_read(mspi->regmap, reg, &val);
		//	printk("xx 0x%08x: 0x%08x\n", reg, val);
		//}
		if (wait_event_timeout(mspi->wait, mspi->tfrdone, HZ / 100) == 0) {
			unsigned int done;

			regmap_field_read(mspi->done, &done);
			dev_err(mspi->dev, "timeout waiting for transfer to complete, txbuf %d,"
					   "rxbuf %d, txlen %d, blksz %d, done %d, cpu %d, hz %d\n",
					txbuf ? 1 : 0, rxbuf ? 1 : 0, transfer->len, blksz,
							done, smp_processor_id(), transfer->speed_hz);

			return -EIO;
		}

		/* save any incoming data into the buffer */
		if (rxbuf) {
			msc313_spi_saverxbuff(mspi, rdbuf, rxbuf, blksz);
			rxbuf += blksz;
		}

		txed += blksz;
	}

	return 0;
}

static irqreturn_t msc313_spi_irq(int irq, void *data)
{
	struct msc313_spi *spi = data;
	unsigned int done;

	regmap_field_read(spi->done, &done);
	if (!done)
		return IRQ_NONE;

	regmap_field_force_write(spi->doneclr, 1);
	spi->tfrdone = true;
	wake_up(&spi->wait);

	return IRQ_HANDLED;
}

static const struct clk_div_table div_table[] = {
	{0, 2},
	{1, 4},
	{2, 8},
	{3, 16},
	{4, 32},
	{5, 64},
	{6, 128},
	{7, 256},
	{ 0 },
};

static int msc313_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_master *master;
	int ret, irq, numparents;
	struct msc313_spi *spi;
	const char *parents[1];
	void __iomem *base;
	char *sclk_name;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, ARRAY_SIZE(parents));
	if (numparents != 1)
		return -EINVAL;

	master = spi_alloc_master(&pdev->dev, sizeof(struct msc313_spi));
	if (!master)
		return -ENOMEM;

	platform_set_drvdata(pdev, master);

	spi = spi_master_get_devdata(master);
	init_waitqueue_head(&spi->wait);
	spi->dev = &pdev->dev;
	spi->master = master;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	//spi->dmachan = dma_request_chan(&pdev->dev, "movedma");
	//if (IS_ERR(spi->dmachan)) {
	//	dev_warn(&pdev->dev, "failed to request dma channel: %ld, will use cpu!\n",
	//		 PTR_ERR(spi->dmachan));
	//	spi->dmachan = NULL;
	//                                                                                                                               }

	spi->regmap = devm_regmap_init_mmio(spi->dev, base,
			&msc313_spi_regmap_config);
	if (IS_ERR(spi->regmap))
		return PTR_ERR(spi->regmap);

	spi->wrsz = devm_regmap_field_alloc(spi->dev, spi->regmap, size_write_field);
	spi->rdsz = devm_regmap_field_alloc(spi->dev, spi->regmap, size_read_field);
	spi->cpha = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_cpha_field);
	spi->cpol = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_cpol_field);
	spi->trigger = devm_regmap_field_alloc(spi->dev, spi->regmap, trigger_field);
	spi->done = devm_regmap_field_alloc(spi->dev, spi->regmap, done_done_field);
	spi->doneclr = devm_regmap_field_alloc(spi->dev, spi->regmap, doneclr_field);
	spi->cs = devm_regmap_field_alloc(spi->dev, spi->regmap, cs_field);


	init_waitqueue_head(&spi->dma_wait);
	spi->dmaen = devm_regmap_field_alloc(spi->dev, spi->regmap, dmaen_field);
	spi->dmarw = devm_regmap_field_alloc(spi->dev, spi->regmap, dmarw_field);

	sclk_name = devm_kasprintf(dev, GFP_KERNEL, "%s_sclk", dev_name(dev));
	spi->divider = devm_clk_hw_register_divider_table(dev, sclk_name, parents[0], 0,
			base + REG_CTRL, DIV_SHIFT, DIV_WIDTH,
			0, div_table, &spi->lock);

	ret = clk_prepare_enable(spi->divider->clk);
	if (ret)
		dev_err(&pdev->dev, "clk enable failed: %d\n", ret);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_spi_irq, IRQF_SHARED,
			dev_name(&pdev->dev), spi);

	master->dev.of_node = pdev->dev.of_node;
	master->bus_num = pdev->id;
	master->num_chipselect = 1;
	master->mode_bits = SPI_CPHA | SPI_CPOL;
	master->max_speed_hz = clk_hw_round_rate(spi->divider, ~0);
	master->min_speed_hz = clk_hw_round_rate(spi->divider, 0);
	master->setup = msc313_spi_setup;
	master->set_cs = msc313_spi_set_cs;
	master->transfer_one = msc313_spi_transfer_one;
	master->bits_per_word_mask = SPI_BPW_MASK(8);

	ret = regmap_update_bits(spi->regmap, REG_CTRL,
		CTRL_ENABLE | CTRL_RESET | CTRL_INT,
		CTRL_ENABLE | CTRL_RESET | CTRL_INT);

	return devm_spi_register_master(&pdev->dev, master);
}

static const struct of_device_id msc313_spi_of_match[] = {
	{ .compatible = "mstar,msc313-spi", },
	{ .compatible = "sstar,ssd20xd-spi", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_spi_of_match);

static struct platform_driver msc313_spi_driver = {
	.probe = msc313_spi_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_spi_of_match,
	},
};
module_platform_driver(msc313_spi_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 SPI driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
