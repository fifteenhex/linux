// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

/*
 * The vendor source for the SPI NOR interface has register definitions
 * for tons of different things so it's hard to work out what is actually
 * in the chip and what is a left over from another driver they copied and
 * hacked up. I know for sure the ISP block exists and works as a sort of
 * SPI NOR focused SPI master controller. Another block that is mentioned
 * in the driver is FSP which seems to be another SPI master controller but
 * with big transaction buffers so that you can read and write in large chunks.
 * The registers for that block don't seem to be writable though.
 * Somehow the SPI NOR is also readable via a memory mapped area. I'm not sure
 * how that actually works or where it's configured. It seems to be a function of
 * the FSP block as the vendor driver maps the memory mapped region based on a define
 * that selects the FSP "direct read mode".
 * The bootrom seems to use it to load the IPL so it might be configured right
 * after exiting reset.
 *
 * ISP registers
 *
 * 0x00 - password
 * 0x04 - cmd
 * 0x10 - wdata - data to be written
 * 0x14 - rdata - data to be read
 * 0x18 - clkdiv?
 * 0x20 - ceclr - chip select control
 * 0x30 - rdreq - read trigger
 * 0x54 - rd_datardy - read data poll
 * 0x58 - wr_datardy - write data poll
 * 0x7e - reset
 *   2
 * ~rst
 * 0xa8 - trigger mode
 *
 * 0xfc - rst
 *
 *    2     |
 *   rst    |
 * 0 - rst  |
 * 1 - nrst |
 *
 * FSP
 *
 * The idea of the FSP seems to be to allow fast page writes.
 * You load a page of data into it's buffer, setup some bytes
 * that go before and after it for the flash protocol and then
 * fire the whole lot off.
 *
 * 0x0
 *  .
 *  . Seems to be a 256 byte buffer that is writable by
 *  . the bdma controller
 *  .
 * 0xFF
 *
 * 0x1b0 - ctrl
 *
 * |  3   |    2   |   1  |  0
 * | chk? | int en | ~rst | en
 *
 * 0x1b4 - trigger
 *    0
 * trigger
 *
 * 0x1b8 - status
 *   0
 * done
 *
 * QSPI register
 *
 * 0x01c - pm code: spi_arb_ctrl[1:0]
 * 0x020 - pm code: non_pm_ack_timeout_len[15:0]
 *
 * 0x028 - burst write control
 * 0x1 enable burst
 * 0x2 disable burst
 *
 * 0x150 - wrap val
 *
 * 0x1c0 - ckg spi
 *
 * 0x1c4 - chip select timing
 *
 * 0x1c8 - Read mode
 *
 *        3 - 0
 * val | mode   | spi command
 * 0x0 |        | 0x03
 * 0x1 | fast   | 0x0b
 * 0x2 | 1_1_2  | 0x3b
 * 0x3 | 1_2_2  | 0xbb
 * 0xa | 1_1_4  | 0x6b
 * 0xb | 1_4_4  | 0xeb
 * 0xc |        | 0x0b
 * 0xd | 4eb?   | ?
 *
 * 0x1e8 - chip select
 *
 * 0x1f4 - function select
 *    13   |    12    |    11    |
 * wrap_en | dummy_en | addr2_en |
 */

#define DRIVER_NAME					"msc313-isp"

#define REG_PASSWORD				0x0
#define VAL_PASSWORD_UNLOCK			0xAAAA
#define VAL_PASSWORD_LOCK			0x5555
#define REG_SPI_WDATA				0x10
#define REG_SPI_RDATA				0x14
#define REG_SPI_CECLR				0x20
#define REG_SPI_RDREQ				0x30
#define REG_SPI_RD_DATARDY			0x54
#define BIT_SPI_RD_DATARDY_READY	BIT(0)
#define REG_SPI_WR_DATARDY			0x58
#define BIT_SPI_WR_DATARDY_READY	BIT(0)
#define REG_TRIGGER_MODE			0xa8
#define REG_RST						0xfc
#define VAL_TRIGGER_MODE_ENABLE		0x3333
#define VAL_TRIGGER_MODE_DISABLE	0x2222

//unused
#define REG_SPI_COMMAND				(0x1 * REG_OFFSET)
#define REG_SPI_ADDR_L				(0x2 * REG_OFFSET)
#define REG_SPI_ADDR_H				(0x3 * REG_OFFSET)

static const struct regmap_config msc313_isp_regmap_config = {
                .name = "msc313-isp",
                .reg_bits = 16,
                .val_bits = 16,
                .reg_stride = 4,
};

static struct reg_field rst_nrst_field = REG_FIELD(REG_RST, 2, 2);
static struct reg_field readreq_req_field = REG_FIELD(REG_SPI_RDREQ, 0, 0);
static struct reg_field rdata_field = REG_FIELD(REG_SPI_RDATA, 0, 7);
static struct reg_field rddatardy_ready_field = REG_FIELD(REG_SPI_RD_DATARDY, 0, 0);
static struct reg_field wdata_field = REG_FIELD(REG_SPI_WDATA, 0, 7);
static struct reg_field wrdatardy_ready_field = REG_FIELD(REG_SPI_WR_DATARDY, 0, 0);
static struct reg_field ceclr_clear_field = REG_FIELD(REG_SPI_CECLR, 0, 0);

struct msc313_isp {
	struct device *dev;
	struct spi_master *master;
	struct clk *clk;
	struct regmap *regmap;
	void __iomem *base;
	void __iomem *memorymapped;
	struct dma_chan *dmachan;

	struct regmap_field *nrst;
	struct regmap_field *rdreq;
	struct regmap_field *rdata;
	struct regmap_field *rddatardy;
	struct regmap_field *wdata;
	struct regmap_field *wrdatardy;
	struct regmap_field *ceclr;

	wait_queue_head_t dma_wait;
	bool dma_done;
	bool dma_success;
};

#define MSC313_ISP_DEBUG

static void msc313_isp_enable(struct msc313_isp *isp){
	//regmap_field_force_write(isp->nrst, 0);
	//regmap_field_force_write(isp->nrst, 1);
	iowrite16(VAL_PASSWORD_UNLOCK, isp->base + REG_PASSWORD);
	iowrite16(VAL_TRIGGER_MODE_ENABLE, isp->base + REG_TRIGGER_MODE);
}

static void msc313_isp_disable(struct msc313_isp *isp){
	iowrite16(VAL_TRIGGER_MODE_DISABLE, isp->base + REG_TRIGGER_MODE);
	iowrite16(VAL_PASSWORD_LOCK, isp->base + REG_PASSWORD);
	//regmap_field_force_write(isp->nrst, 0);
}

static void msc313_isp_spi_writebyte(struct msc313_isp *isp, u8 value){
	unsigned int rdyval;
	regmap_field_force_write(isp->wdata, value);
	if(regmap_field_read_poll_timeout(isp->wrdatardy, rdyval,
			rdyval & BIT_SPI_WR_DATARDY_READY, 0, 1000000)){
		dev_err(isp->dev, "write timeout");
	}
}

static void msc313_isp_spi_readbyte(struct msc313_isp *isp, u8 *dest){
	unsigned int rdyval;
	unsigned int regval;
	regmap_field_force_write(isp->rdreq, 1);
	if(regmap_field_read_poll_timeout(isp->rddatardy, rdyval,
				rdyval & BIT_SPI_RD_DATARDY_READY, 0, 1000000)){
		dev_err(isp->dev, "read timeout");
	}
	regmap_field_read(isp->rdata, &regval);
	*dest = (u8) regval;
}

static void msc313_isp_spi_clearcs(struct msc313_isp *isp){
	regmap_field_force_write(isp->ceclr, 1);
}

static int msc313_isp_setup(struct spi_device *spi){
	// does nothing for now
	return 0;
}

static int msc313_isp_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
			    struct spi_transfer *transfer){
	struct msc313_isp *isp = spi_controller_get_devdata(ctlr);
	const u8 *tx_buf = transfer->tx_buf;
	u8 *rx_buf = transfer->rx_buf;
	unsigned b = 0;

	// this only really works for SPI NOR <cs low><write something><read something><cs high> transactions

	for(b = 0; b < transfer->len; b++) {
		if (tx_buf) {

			msc313_isp_spi_writebyte(isp, *tx_buf++);
			if(rx_buf){
				// we don't do full duplex, there should be no rx buffer
			}
		}
		else if(rx_buf){
			msc313_isp_spi_readbyte(isp, rx_buf++);
		}
	}

	return 0;
}

static void msc313_isp_set_cs(struct spi_device *spi, bool enable){
	// cs is asserted by the controller, we can only deassert it
	struct msc313_isp *isp = spi_master_get_devdata(spi->master);
	if(!enable)
		msc313_isp_spi_clearcs(isp);
}


static int msc313_isp_spi_mem_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op){
	return 0;
}

static bool msc313_isp_spi_mem_supports_op(struct spi_mem *mem,
			    const struct spi_mem_op *op){
	return true;
}

static int msc313_isp_exec_op(struct spi_mem *mem,
		       const struct spi_mem_op *op){
	struct msc313_isp *isp = spi_controller_get_devdata(mem->spi->controller);
	int i, ret = 0;

	if(op->cmd.opcode)
		msc313_isp_spi_writebyte(isp, op->cmd.opcode);
	if(op->addr.nbytes != 0)
		for(i = op->addr.nbytes; i > 0; i--)
			msc313_isp_spi_writebyte(isp, (op->addr.val >> (8 * (i - 1))) & 0xff);
	if(op->dummy.nbytes != 0)
		for(i = 0; i < op->dummy.nbytes; i++)
			msc313_isp_spi_writebyte(isp, 0xff);
	switch(op->data.dir){
		case SPI_MEM_DATA_IN:
			for(i = 0; i < op->data.nbytes; i++)
				msc313_isp_spi_readbyte(isp, ((u8*)(op->data.buf.in + i)));
			break;
		case SPI_MEM_DATA_OUT:
			for(i = 0; i < op->data.nbytes; i++)
				msc313_isp_spi_writebyte(isp, *((u8*)(op->data.buf.out + i)));
			break;
		case SPI_MEM_NO_DATA:
			break;
	}

	msc313_isp_spi_clearcs(isp);
	return ret;
}

static int msc313_isp_spi_mem_dirmap_create(struct spi_mem_dirmap_desc *desc){
	if(desc->info.op_tmpl.data.dir == SPI_MEM_DATA_IN)
		desc->nodirmap = 0;
	else
		desc->nodirmap = 1;
	return 0;
}

static void msc313_isp_spi_mem_dirmap_destroy(struct spi_mem_dirmap_desc *desc){
	// nothing for now
}

static void msc313_isp_dma_callback(void *dma_async_param,
				const struct dmaengine_result *result){
	struct msc313_isp *isp = dma_async_param;
	isp->dma_done = true;
	isp->dma_success = result->result == DMA_TRANS_NOERROR;
	if(!isp->dma_success)
		dev_err(&isp->master->dev, "dma failed: %d\n", result->result);
	wake_up(&isp->dma_wait);
}


static ssize_t msc313_isp_spi_mem_dirmap_read(struct spi_mem_dirmap_desc *desc,
		u64 offs, size_t len, void *buf){
	struct msc313_isp *isp = spi_controller_get_devdata(desc->mem->spi->controller);
	struct dma_async_tx_descriptor *dmadesc;
	struct dma_slave_config config;
	dma_addr_t dmaaddr = 0;

	msc313_isp_disable(isp);

	if(isp->dmachan){
		dmaaddr = dma_map_single(&isp->master->dev, buf, len, DMA_FROM_DEVICE);
		isp->dma_done = false;
		config.slave_id = 0x5;
		config.src_addr = offs;
		config.src_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
		dmaengine_slave_config(isp->dmachan, &config);
		dmadesc = dmaengine_prep_slave_single(isp->dmachan, dmaaddr, len, DMA_DEV_TO_MEM, 0);
		if(dmadesc){
			dmadesc->callback_result = msc313_isp_dma_callback;
			dmadesc->callback_param = isp;
			dmaengine_submit(dmadesc);
			dma_async_issue_pending(isp->dmachan);
			wait_event(isp->dma_wait, isp->dma_done);
			rmb();
			if(isp->dma_success)
				goto dma_success;
			else
				dev_warn(isp->dev, "dma failed, falling back to cpu read\n");
		}
	}

	// despite the documentation saying not to do this on the
	// cpu we don't have a lot of choice if dma isn't working
	memcpy(buf, isp->memorymapped + offs, len);
dma_success:
	if(dmaaddr)
		dma_unmap_single(&isp->master->dev, dmaaddr, len, DMA_FROM_DEVICE);
	msc313_isp_enable(isp);
	return len;
}

static struct spi_controller_mem_ops msc313_isp_mem_ops = {
		.adjust_op_size = msc313_isp_spi_mem_adjust_op_size,
		.supports_op = msc313_isp_spi_mem_supports_op,
		.exec_op = msc313_isp_exec_op,
		.dirmap_create = msc313_isp_spi_mem_dirmap_create,
		.dirmap_destroy = msc313_isp_spi_mem_dirmap_destroy,
		.dirmap_read = msc313_isp_spi_mem_dirmap_read
};

static int msc313_isp_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct msc313_isp *isp;
	int ret;
	void __iomem *base;
	u32 max_freq;

	master = spi_alloc_master(&pdev->dev, sizeof(struct msc313_isp));
	if (!master) {
		dev_err(&pdev->dev, "spi master allocation failed\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, master);

	isp = spi_master_get_devdata(master);
	isp->dev = &pdev->dev;
	isp->master = master;

	base = of_iomap(pdev->dev.of_node, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	isp->base = base;

	isp->regmap = devm_regmap_init_mmio(&pdev->dev, base,
			&msc313_isp_regmap_config);
	isp->nrst = devm_regmap_field_alloc(&pdev->dev, isp->regmap, rst_nrst_field);
	isp->rdreq = devm_regmap_field_alloc(&pdev->dev, isp->regmap, readreq_req_field);
	isp->rdata = devm_regmap_field_alloc(&pdev->dev, isp->regmap, rdata_field);
	isp->rddatardy = devm_regmap_field_alloc(&pdev->dev, isp->regmap, rddatardy_ready_field);
	isp->wdata = devm_regmap_field_alloc(&pdev->dev, isp->regmap, wdata_field);
	isp->wrdatardy = devm_regmap_field_alloc(&pdev->dev, isp->regmap, wrdatardy_ready_field);
	isp->ceclr = devm_regmap_field_alloc(&pdev->dev, isp->regmap, ceclr_clear_field);

	isp->memorymapped = of_iomap(pdev->dev.of_node, 3);
	if (IS_ERR(isp->memorymapped))
			return PTR_ERR(isp->memorymapped);

	isp->dmachan = dma_request_chan(&pdev->dev, "qspi");
	if(IS_ERR(isp->dmachan)){
		dev_warn(&pdev->dev, "failed to request dma channel: %ld, will use cpu!\n", PTR_ERR(isp->dmachan));
		isp->dmachan = NULL;
	}

	isp->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(isp->clk)) {
			return PTR_ERR(isp->clk);
	}

	master->dev.of_node = pdev->dev.of_node;
	master->bus_num = pdev->id;
	master->num_chipselect = 1;
	master->mode_bits = SPI_CPHA | SPI_CPOL;
	master->max_speed_hz = 50000;
	master->min_speed_hz = 50000;
	master->flags = SPI_CONTROLLER_HALF_DUPLEX;
	master->setup = msc313_isp_setup;
	master->transfer_one = msc313_isp_transfer_one;
	master->set_cs = msc313_isp_set_cs;
	master->mem_ops = &msc313_isp_mem_ops;

	init_waitqueue_head(&isp->dma_wait);

	ret = clk_prepare_enable(isp->clk);

	if (of_property_read_u32(pdev->dev.of_node, "spi-max-frequency", &max_freq) == 0)
		clk_set_rate(isp->clk, max_freq);

	msc313_isp_enable(isp);

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret) {
		dev_err(&pdev->dev, "spi master registration failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static int msc313_isp_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id msc313_isp_match[] = {
	{.compatible = "mstar,msc313-isp"},
	{}
};
MODULE_DEVICE_TABLE(of, msc313_isp_match);

static struct platform_driver msc313_isp_driver = {
	.probe	= msc313_isp_probe,
	.remove	= msc313_isp_remove,
	.driver	= {
		.name = DRIVER_NAME,
		.of_match_table = msc313_isp_match,
	},
};
module_platform_driver(msc313_isp_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("MStar MSC313 ISP driver");
MODULE_LICENSE("GPL v2");
