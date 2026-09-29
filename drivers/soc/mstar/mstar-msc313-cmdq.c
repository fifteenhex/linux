// SPDX-License-Identifier: GPL-2.0
//

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of_irq.h>

/*
 *
 * MSC313 CMDQ DMA controller
 *
 * The MSC313 has 1 of these. The MSC313e seems to have 3. The SSD20xD has
 * exactly one: the vendor's HAL_CMDQ_Get_Cmdq_RiuAddr() rejects any index but 0,
 * and the MIU has a single CMDQ0_R client. Its interrupt there is GIC SPI 49
 * (from the vendor device tree's cmdq0 node, which carries no reg property - the
 * register base is hardcoded in the vendor's mhal, so it is still unknown here).
 *
 * The vendor SDK seems to mostly use it for moving stuff to and from
 * the camera ip blocks. It writes registers, waits for a trigger event and
 * polls a register until it matches or stops matching, which makes it a way to
 * hand a whole batch of register writes to the hardware and have them applied
 * without the CPU - synchronised to something, e.g. a vsync.
 *
 * Descriptors are 8 bytes. Confirmed against the vendor's builders
 * (_MDrvCmdqInsertOneCommand writes the eight bytes one at a time, and
 * _MDrvCmdqInsertOneWriteCommand assembles the arguments for a write), so as a
 * little endian u64:
 *
 *   bits 15:0   ~mask   the mask is stored INVERTED: a set bit means leave
 *                       that bit alone. mvns in the write builder.
 *   bits 31:16  data
 *   bits 55:32  addr    RIU *word* address, i.e. the byte address >> 1, 24
 *                       bits. The builder does ubfx(addr, 1, 23).
 *   bits 63:56  cmd     the whole top byte, not a nibble:
 *                         0x00 - nop / dummy
 *                         0x10 - write
 *                         0x20 - wait for a trigger event
 *                         0x30 - poll until equal
 *                         0xb0 - poll until not equal
 *                       (0x30 vs 0xb0 is one flag in
 *                       MDrvCmdqPollEqCommandMask; the low nibble is unused.)
 *
 * The queue is a ring of these: the vendor advances its write pointer by 8 per
 * command and wraps against the buffer end, and MDrvCmdqCheckBufferAvail() is
 * what keeps it from overrunning the part the engine has not consumed yet.
 *
 * 0x004 -
 * 0
 * en
 *
 * 0x008 - dma mode
 * 0x00 - increment mode
 * 0x01 - direct mode
 * 0x04 - ring mode
 *
 * 0x00c - trigger?
 *   1
 * start?
 *
 * 0x010 - start pointer
 * 0x018 - end pointer
 * 0x020 - offset pointer
 * 0x040 - miu sel
 * 0x044 - ??
 * 0x080 - ??
 * 0x088 - wait trig
 * 0x090 -
 * 0x0a0 - timeout
 * 0x0a4 - ""
 * 0x0c4 - reset
 *  0
 * ~rst
 *
 * 0x100 - something to do with errors
 * 0x10c - ""
 * 0x110 - something to do with irq
 * 0x11c - irq mask
 * 0x120 - irq clear
 * 0x128 - timer
 * 0x12c - ratio
 *
 */

#define DRIVER_NAME "msc313-cmdq"
#define CHANNELS 1

#define REG_ENABLE		0x4
static struct			reg_field enable_en_field = REG_FIELD(REG_ENABLE, 0, 0);

#define REG_TRIG0		0x8
static struct reg_field		dma_trig_en_field = REG_FIELD(REG_TRIG0, 0, 0);
static struct reg_field		buff_mode_field = REG_FIELD(REG_TRIG0, 1, 2);

#define REG_TRIG1		0xc
static struct reg_field		dma_trig_field = REG_FIELD(REG_TRIG1, 0, 0);
static struct reg_field		mov_cmd_ptr_field = REG_FIELD(REG_TRIG1, 1, 1);
static struct reg_field		rst_cmd_st_ptr_trig_field = REG_FIELD(REG_TRIG1, 3, 3);

#define REG_CMD_ST_PTR0		0x10
#define REG_CMD_ST_PTR1		0x14
#define REG_CMD_END_PTR0	0x18
#define REG_CMD_END_PTR1	0x1c


#define REG_SKIPFORCE		0x90
static struct reg_field		skip_wr_field = REG_FIELD(REG_SKIPFORCE, 0, 0);
static struct reg_field		skip_wait_field = REG_FIELD(REG_SKIPFORCE, 1, 1);
static struct reg_field		skip_polleq_field = REG_FIELD(REG_SKIPFORCE, 2, 2);
static struct reg_field		skip_pollneq_field = REG_FIELD(REG_SKIPFORCE, 3, 3);
static struct reg_field		skip_wr_mask_field = REG_FIELD(REG_SKIPFORCE, 4, 4);
static struct reg_field		skip_wait_mask_field = REG_FIELD(REG_SKIPFORCE, 5, 5);

#define REG_RESET		0x0c4
static struct reg_field		rst_nrst_field = REG_FIELD(REG_RESET, 0, 0);

#define REQ_CRASH0		0x108
#define REQ_CRASH1		0x10c

#define REG_RAW_IRQ_FINAL_IRQ	0x110
static struct reg_field		cmdq_done_field = REG_FIELD(REG_RAW_IRQ_FINAL_IRQ, 3, 3);
static struct reg_field		soft_inter_field = REG_FIELD(REG_RAW_IRQ_FINAL_IRQ, 4, 7);

#define REG_IRQ_FORCE		0x118
#define REG_IRQ_MASK		0x11c
#define REG_IRQ_CLEAR		0x120

static const struct regmap_config msc313_cmdq_regmap_config = {
	.name = DRIVER_NAME,
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

struct msc313_cmdq {
	struct clk *clk;
	struct regmap *regmap;
	struct regmap_field *nrst;
};

static const struct of_device_id msc313_cmdq_of_match[] = {
	{ .compatible = "mstar,msc313-cmdq", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_cmdq_of_match);

static irqreturn_t msc313_cmdq_irq(int irq, void *data)
{
	struct msc313_cmdq *cmdq = data;

	printk("%s:%d\n", __func__, __LINE__);

	regmap_write(cmdq->regmap, REG_IRQ_FORCE, 0);
	regmap_write(cmdq->regmap, REG_IRQ_CLEAR, ~0);

	return IRQ_HANDLED;
}

static int msc313_cmdq_probe(struct platform_device *pdev)
{
	struct msc313_cmdq *cmdq;
	struct device *dev = &pdev->dev;
	struct msc313_cmdq_chan *chan;
	void __iomem *base;
	int i, ret;
	int irq;

	printk("cmdq probe\n");

	cmdq = devm_kzalloc(&pdev->dev, sizeof(*cmdq), GFP_KERNEL);
	if (!cmdq)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	cmdq->regmap = devm_regmap_init_mmio(dev, base,
			&msc313_cmdq_regmap_config);
	if (IS_ERR(cmdq->regmap))
		return PTR_ERR(cmdq->regmap);

	cmdq->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(cmdq->clk)) {
		return PTR_ERR(cmdq->clk);
	}

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -EINVAL;
	ret = devm_request_irq(dev, irq, msc313_cmdq_irq,
			IRQF_SHARED, dev_name(dev), cmdq);

	cmdq->nrst = devm_regmap_field_alloc(dev, cmdq->regmap, rst_nrst_field);

	ret = clk_prepare_enable(cmdq->clk);
	if (ret)
		goto out;

out:
	return ret;
}

static struct platform_driver msc313_cmdq_driver = {
	.probe = msc313_cmdq_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_cmdq_of_match,
	},
};
module_platform_driver(msc313_cmdq_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 CMDQ driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
