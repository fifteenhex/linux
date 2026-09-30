// SPDX-License-Identifier: GPL-2.0
//

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/*
 *
 * MSC313 CMDQ DMA controller
 *
 * The MSC313 has 1 of these. The MSC313e seems to have 3. The SSD20xD has
 * exactly one: the vendor's HAL_CMDQ_Get_Cmdq_RiuAddr() rejects any index but 0,
 * and the MIU has a single CMDQ0_R client. Its interrupt is GIC SPI 49, from the
 * vendor device tree's cmdq0 node, which carries no reg property because the
 * base is compiled into mhal: gHalRegCmdCtlBase in its .data is 0x00112000, a
 * RIU word address, so byte 0x224000 and 0x1f224000 to the CPU - the same place
 * the MSC313 has it. Confirmed on an SSD202D: that block reads a sane reset
 * state, with the reset bit at 0x0c4 released and the timeouts at 0x0a0/0x0a4
 * holding 0xffff/0x1080.
 *
 * The two dummy registers and the trigger clear that the vendor hands out as
 * targets for a command to poke are base | 0xf0, | 0xee and | 0xa8 in word
 * addresses - byte 0x1e0, 0x1dc and 0x150 - from
 * HAL_CMDQ_Get_Dummy_Register_RiuAddr(), _Dummy2_ and _TriggerClr_.
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
 * The ring pointers, from mhal's own accessors:
 *
 * 0x010/0x014 - start pointer        HAL_CMDQ_Set_Start_Pointer
 * 0x018/0x01c - end pointer          HAL_CMDQ_Set_End_Pointer, and
 *                                    HAL_CMDQ_Set_Offset_Pointer writes the same
 *                                    pair, so this is the producer's position
 *                                    that the engine is chasing rather than a
 *                                    fixed limit
 * 0x020/0x024 - write pointer        HAL_CMDQ_Get_Write_Pointer, read only:
 *                                    where the engine itself has got to
 *
 * That is worth knowing because 0x020 looks like a pointer to program and is
 * not. It also means the pointer unit cannot be the sixteen byte one the rest of
 * this chip uses, since that could not address an eight byte descriptor
 * boundary.
 *
 * What has been tried and does not start it, on an SSD202D: the reset released,
 * interrupts masked, 0x004 enable set, 0x008 given 0, 1, 3, 4 and 5, the start
 * and end pointers written in byte, eight byte and sixteen byte units, 0x00c bit
 * 3 pulsed to load the start pointer and then bit 0 pulsed to go. Every register
 * reads back what was written and 0x00c self clears, so the block is clocked and
 * accepting configuration, but the write pointer never moves off zero, the done
 * bit in 0x110 never sets and the two registers at 0x108/0x10c stay clear. The
 * descriptors themselves are well formed - 0x101120f0be000000 writes 0xbe00 to
 * the engine's own dummy register. Something else is needed to arm it; a trace of
 * the vendor stack driving it would settle it.
 *
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
#define REG_CMD_WR_PTR0		0x20	/* read only, where the engine has got to */
#define REG_CMD_WR_PTR1		0x24


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

/*
 * Leave the engine reset and silent.
 *
 * The interrupt is deliberately not requested. The vendor device tree gives
 * cmdq0 GIC SPI 49, but in this tree that line is the MIU's - see the miu node
 * in mstar-v7.dtsi - and it is asserted permanently with nothing to clear it,
 * which is why the MIU driver's own devm_request_irq() is commented out.
 * Registering a handler there enables the line and the box does nothing but
 * service it: measured as one interrupt per console line, i.e. as fast as the
 * handler could be printed. Until the real number is known, completion is
 * polled through REG_RAW_IRQ_FINAL_IRQ bit 3.
 *
 * Masking this block's own sources as well means it cannot contribute to that
 * line whoever else ends up owning it.
 */
static void msc313_cmdq_hw_init(struct msc313_cmdq *cmdq)
{
	regmap_field_write(cmdq->nrst, 0);
	regmap_field_write(cmdq->nrst, 1);

	regmap_write(cmdq->regmap, REG_IRQ_MASK, 0xffff);
	regmap_write(cmdq->regmap, REG_IRQ_FORCE, 0);
	regmap_write(cmdq->regmap, REG_IRQ_CLEAR, 0xffff);
}

static int msc313_cmdq_probe(struct platform_device *pdev)
{
	struct msc313_cmdq *cmdq;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	int ret;

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
	if (IS_ERR(cmdq->clk))
		return PTR_ERR(cmdq->clk);

	cmdq->nrst = devm_regmap_field_alloc(dev, cmdq->regmap, rst_nrst_field);
	if (IS_ERR(cmdq->nrst))
		return PTR_ERR(cmdq->nrst);

	ret = clk_prepare_enable(cmdq->clk);
	if (ret)
		return ret;

	msc313_cmdq_hw_init(cmdq);

	dev_info(dev, "MStar CMDQ\n");

	return 0;
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
