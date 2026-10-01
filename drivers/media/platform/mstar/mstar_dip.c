// SPDX-License-Identifier: GPL-2.0
/*
 * SigmaStar SSD20xD DIP: a DRAM to DRAM image processor.
 *
 * The vendor stack drives this block through an abstraction it calls DIVP, which
 * is why its own naming is split between "DIP" for the hardware and "DIVP" for
 * the operation. What it is is a memory to memory engine with a read port and a
 * write port, a scaler, a cropper, a horizontal mirror and YUV/RGB conversion in
 * both directions, so V4L2 mem2mem is its natural home.
 *
 * Everything here was recovered from the vendor's mhal, and the parts that are
 * exercised were checked against hardware on a Miyoo Mini:
 *
 *   - a plain NV12 copy comes out byte identical;
 *   - the horizontal mirror comes out reversed, as it should;
 *   - YUV to RGB is a correct BT.601 limited range conversion. Y of 16, 128 and
 *     235 with neutral chroma give 0x00, 0x82 and 0xff, which is 0, 130 and 255,
 *     matching the maths exactly. The result is ARGB8888 with alpha forced on.
 *
 * It does not work yet, and the node is left disabled because of it. Driving the
 * block by poking the same registers from userspace moves data; driving it from
 * here, with every register verified to hold the same value at the moment of the
 * trigger, moves none. The remaining difference has not been found. What has been
 * ruled out:
 *
 *   - the clock, which the clkgen driver now provides and which reads ungated;
 *   - the register programming: a dump of all twenty eight registers that matter,
 *     taken immediately before the trigger, matches the working userspace run
 *     exactly apart from the crop enable, and the userspace run works with the
 *     crop either way;
 *   - the interrupt, which never asserts in either case;
 *   - resetting before each job, which stops it doing anything at all and is now
 *     only done at probe;
 *   - the alignment rules the vendor checks - width even, stride and address both
 *     sixteen byte aligned - all of which are satisfied;
 *   - cache visibility: the kernel's own coherent view of the destination is
 *     untouched too, so the data is genuinely not being written.
 *
 * Two things are wrong with the block even when poked by hand, and both need
 * answering before this can be finished: a frame never runs to completion, and it
 * never signals done - neither the interrupt nor the idle status ever changes - so
 * there is nothing for a driver to wait on. The watchdog here exists for that
 * reason: a job that is never reported finished fails cleanly instead of wedging
 * the queue. The vertical mirror bit also has no observable effect, which is why
 * only V4L2_CID_HFLIP is offered.
 *
 * Register offsets are from the block base. mhal reaches them as byte offsets from
 * 0xfd000000, so anything quoted from it as 0x2477xx is 0x5xx here.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define DRIVER_NAME "mstar-dip"

/*
 * The read port. Addresses are split as bits 19:4 and 31:20 of an address that is
 * relative to the start of DRAM, which is how every master on this SoC takes one.
 */
#define DIP_R_FMT		0x5f4	/* bits 7:6 */
#define DIP_R_HALFWIDTH		0x3b4	/* bits 11:0, stride0 >> 1 */
#define DIP_R_STRIDE0		0x49c	/* bits 11:0 */
#define DIP_R_STRIDE1		0x4dc	/* bits 11:0 */
#define DIP_R_STRIDE2		0x5d0	/* bits 12:0 */
#define DIP_R_ADDR0_LO		0x5e0
#define DIP_R_ADDR0_HI		0x5e4	/* bits 10:0 */
#define DIP_R_ADDR1_LO		0x5e8
#define DIP_R_ADDR1_HI		0x5ec	/* bits 10:0 */
#define DIP_R_SEMIPLANAR	0x41c	/* bit 14 */
#define DIP_R_MISC		0x3b0	/* bits 10:9 */
#define DIP_R_TILEREQ		0x5fc	/* bits 6:0 */

/* The write port, same address encoding. */
#define DIP_W_FMT		0x404	/* bits 5:4 */
#define DIP_W_STRIDE0		0x47c	/* bits 11:0 */
#define DIP_W_STRIDE1		0x4bc	/* bits 11:0 */
#define DIP_W_STRIDE2		0x4fc	/* bits 11:0 */
#define DIP_W_ADDR0_LO		0x440
#define DIP_W_ADDR0_HI		0x444	/* bits 10:0 */
#define DIP_W_ADDR1_LO		0x480
#define DIP_W_ADDR1_HI		0x484	/* bits 10:0 */
#define DIP_W_SEMIPLANAR	0x40c	/* bit 14 */
#define DIP_W_MISC1		0x22c	/* bit 12 */
#define DIP_W_MISC2		0x2c0	/* bit 5 */

/*
 * This one is busy: the low bits are how many MIU write requests may be
 * outstanding, bit 11 is the YUV to RGB enable and bit 13 belongs to one of the
 * 32-bit formats.
 */
#define DIP_W_CTRL		0x5f8
#define  DIP_W_CTRL_TILEREQ	GENMASK(6, 0)
#define  DIP_W_CTRL_Y2R		BIT(11)
#define  DIP_W_CTRL_FMT3	BIT(13)

/* Geometry. For a 1:1 pass every ratio register is simply zero. */
#define DIP_SC_BYPASS		0x2c8	/* bit 12 */
#define DIP_SC_MODE		0x644	/* bits 1:0 */
#define DIP_SC_ENABLE		0x7c0	/* bits 2:1 enable, bit 0 commits */
#define DIP_SC_SRC_W		0x688	/* bits 12:0 */
#define DIP_SC_SRC_H		0x68c	/* bits 11:0 */
#define DIP_SC_DST_W		0x690	/* bits 12:0 */
#define DIP_SC_DST_H		0x694	/* bits 11:0 */
#define DIP_SC_HRATIO_A		0x61c
#define DIP_SC_HRATIO_B		0x620
#define DIP_SC_VRATIO_A		0x624
#define DIP_SC_VRATIO_B		0x628
#define DIP_SC_RATIO_MISC	0x62c

/* Crop. 1 in the low two bits means no crop, 3 means crop to the window. */
#define DIP_CROP_CTRL		0x380
#define  DIP_CROP_OFF		1
#define  DIP_CROP_ON		3
#define DIP_CROP_X0		0x384
#define DIP_CROP_X1		0x388
#define DIP_CROP_Y0		0x38c
#define DIP_CROP_Y1		0x390

#define DIP_MIRROR		0x42c
#define  DIP_MIRROR_H		BIT(9)
#define  DIP_MIRROR_V		BIT(10)
#define DIP_UVSWAP		0x408
#define  DIP_UVSWAP_EN		BIT(14)
#define DIP_R2Y			0x2c4	/* bit 0 */

#define DIP_CTRL		0x5f0
#define  DIP_CTRL_SEMIPLANAR	BIT(3)
#define  DIP_CTRL_TRIGGER	BIT(15)
#define DIP_IDLE		0x3f4
#define DIP_RESET		0x3fc	/* bit 0 */
#define DIP_FRAMECNT		0x760
#define DIP_CHANNEL		0x764

#define DIP_INTR_EN		0x560	/* bits 7:0 */
#define DIP_INTR_EN2		0x5a0	/* bits 7:0 */
#define DIP_INTR_CLEAR		0x564	/* bits 7:0 */
#define DIP_INTR_STATUS		0x568
#define  DIP_INTR_ALL		0xff

/* how many MIU requests each port may have outstanding; the vendor's numbers */
#define DIP_TILEREQ_READ	0x20
#define DIP_TILEREQ_WRITE	0x10

/* a job that never reports done should fail rather than wedge the queue */
#define DIP_JOB_TIMEOUT_MS	300

/*
 * The hardware's format enum. HAL_XC_DIP_GetBPP indexes bytes per pixel with it
 * and gives { 2, 2, 4, 4, 1 }; only the semi-planar one has a second plane, and
 * HAL_XC_DIP_CheckMirrorNeedUVSwap wants the chroma swapped when horizontally
 * mirroring 0 or 4, which is what identifies those two as the YUV ones.
 */
enum dip_fmt {
	DIP_FMT_YUV422	= 0,
	DIP_FMT_RGB565	= 1,
	DIP_FMT_ARGB8888 = 2,
	DIP_FMT_32BIT_ALT = 3,
	DIP_FMT_NV12	= 4,
};

struct mstar_dip_fmt {
	u32 fourcc;
	enum dip_fmt hw;
	u8 bpp;		/* bytes per pixel of plane 0 */
	bool semiplanar;
	bool yuv;
};

static const struct mstar_dip_fmt dip_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.hw = DIP_FMT_NV12,
		.bpp = 1,
		.semiplanar = true,
		.yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_YUYV,
		.hw = DIP_FMT_YUV422,
		.bpp = 2,
		.yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_RGB565,
		.hw = DIP_FMT_RGB565,
		.bpp = 2,
	}, {
		/*
		 * Measured: alpha comes back forced to 0xff, so this is the X
		 * variant as much as the A one. Both are offered because the GE,
		 * which is what would rotate one of these afterwards, takes both.
		 */
		.fourcc = V4L2_PIX_FMT_XRGB32,
		.hw = DIP_FMT_ARGB8888,
		.bpp = 4,
	}, {
		.fourcc = V4L2_PIX_FMT_ARGB32,
		.hw = DIP_FMT_ARGB8888,
		.bpp = 4,
	},
};

/*
 * Widths the hardware cannot do, from the table HAL_XC_DIP_CheckWidthLimit walks.
 * All of them are 1072 or more, so nothing small is affected.
 */
static const u16 dip_bad_widths[] = {
	1072, 1136, 1168, 1264, 1328, 1424, 1552, 1616, 1648, 1712, 1744, 1808,
	2032, 2096, 2192, 2224, 2384, 2416, 2512, 2608, 2672, 2768, 2864, 2896,
	3056, 3088, 3152, 3184, 3376, 3568, 3632, 3664, 3728, 3824,
};

static bool dip_width_ok(unsigned int w)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dip_bad_widths); i++)
		if (dip_bad_widths[i] == w)
			return false;

	return true;
}

struct mstar_dip_q_data {
	const struct mstar_dip_fmt *fmt;
	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int sizeimage;
};

struct mstar_dip {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;

	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m_dev;
	/* serialises a job against the interrupt and against its timeout */
	spinlock_t irqlock;
	struct mutex mutex;
	struct delayed_work watchdog;
	struct mstar_dip_ctx *curr;
};

struct mstar_dip_ctx {
	struct v4l2_fh fh;
	struct mstar_dip *dip;
	struct v4l2_ctrl_handler ctrl_handler;
	struct mstar_dip_q_data out;	/* what userspace feeds in */
	struct mstar_dip_q_data cap;	/* what comes back */
	bool hflip;
};

/*
 * HAL_XC_DIP_Init. Sixty one writes, extracted from mhal with nothing left
 * unresolved. It leaves a 720x576 geometry behind, which every job overwrites.
 * Running this is what makes the block do any work at all.
 *
 * Not a reg_sequence: a third of these are read-modify-writes and that struct has
 * no mask field, only a delay. Getting that wrong turns every masked entry into a
 * full register write, which quietly wipes the bits it was supposed to preserve -
 * 0x5f4 ends up 0x4000 instead of 0x4fd8 and 0x408 ends up 0 instead of 0xff - and
 * the block then accepts everything and moves nothing.
 */
struct dip_reg_init {
	u16 reg;
	u16 val;
	u16 mask;
};

static const struct dip_reg_init dip_init[] = {
	{ 0x210, 0x0000, 0xffff }, { 0x214, 0x0000, 0xffff },
	{ 0x220, 0x0000, 0xffff }, { 0x224, 0x0000, 0xffff },
	{ 0x22c, 0x0000, 0xffff }, { 0x240, 0x3fc2, 0x3f00 },
	{ 0x2c0, 0x0020, 0xffff }, { 0x2c4, 0x0000, 0xffff },
	{ 0x3a0, 0x0000, 0xffff }, { 0x3b0, 0x1000, 0xffff },
	{ 0x3b4, 0x8168, 0xffff }, { 0x3bc, 0x0003, 0xffff },
	{ 0x3fc, 0x2040, 0xffff }, { 0x404, 0x0830, 0xffff },
	{ 0x408, 0x04ff, 0xffff }, { 0x40c, 0x4000, 0xffff },
	{ 0x440, 0x0000, 0xffff }, { 0x444, 0x0030, 0xffff },
	{ 0x480, 0x0000, 0xffff }, { 0x484, 0x0040, 0xffff },
	{ 0x47c, 0x02d0, 0xffff }, { 0x4bc, 0x0240, 0xffff },
	{ 0x4fc, 0x02d0, 0xffff }, { 0x588, 0x00ff, 0xffff },
	{ 0x5bc, 0x400a, 0xffff }, { 0x5d4, 0x003f, 0xffff },
	{ 0x5f8, 0x0010, 0xffff }, { 0x45c, 0x0800, 0x0800 },
	{ 0x41c, 0x4000, 0xffff }, { 0x49c, 0x02d0, 0xffff },
	{ 0x4dc, 0x0240, 0xffff }, { 0x5d0, 0x02d0, 0xffff },
	{ 0x5e0, 0x0000, 0xffff }, { 0x5e4, 0x0010, 0xffff },
	{ 0x5e8, 0x0000, 0xffff }, { 0x5ec, 0x0020, 0xffff },
	{ 0x5f0, 0x0008, 0xffff }, { 0x5f4, 0x0fd8, 0xffff },
	{ 0x5fc, 0x402c, 0xffff }, { 0x42c, 0x0080, 0xffff },
	{ 0x61c, 0x0000, 0xffff }, { 0x620, 0x0000, 0xffff },
	{ 0x624, 0x0000, 0xffff }, { 0x628, 0x0000, 0xffff },
	{ 0x62c, 0x0000, 0xffff }, { 0x688, 0x0000, 0xffff },
	{ 0x68c, 0x0000, 0xffff }, { 0x690, 0x0000, 0xffff },
	{ 0x694, 0x0000, 0xffff }, { 0x7c0, 0x0006, 0xffff },
	{ 0x408, 0x0000, 0x0400 }, { 0x5f4, 0x4000, 0x4000 },
	{ 0x420, 0x00ff, 0x00ff }, { 0x4e0, 0x00ff, 0x00ff },
	{ 0x4a0, 0x00ff, 0x00ff }, { 0x520, 0x00ff, 0x00ff },
	{ 0x5a0, 0x0000, 0x00ff }, { 0x430, 0x0000, 0x1000 },
	{ 0x7c0, 0x0004, 0x0006 }, { 0x7c0, 0x0001, 0x0001 },
	{ 0x7c0, 0x0000, 0x0001 },
};

static inline void dip_update(struct mstar_dip *dip, unsigned int reg,
			      unsigned int mask, unsigned int val)
{
	regmap_update_bits(dip->regmap, reg, mask, val);
}

static const struct mstar_dip_fmt *dip_find_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dip_formats); i++)
		if (dip_formats[i].fourcc == fourcc)
			return &dip_formats[i];

	return NULL;
}

static void dip_q_data_set_fmt(struct mstar_dip_q_data *q,
			       const struct mstar_dip_fmt *fmt,
			       unsigned int width, unsigned int height)
{
	q->fmt = fmt;
	q->width = width;
	q->height = height;
	/*
	 * The write port rounds its stride up to a multiple of sixteen, so give
	 * userspace the same thing on both queues and there is no surprise.
	 */
	q->bytesperline = ALIGN(width * fmt->bpp, 16);
	q->sizeimage = q->bytesperline * height;
	if (fmt->semiplanar)
		q->sizeimage += q->bytesperline * height / 2;
}

/*
 * Addresses go in relative to the start of DRAM in sixteen byte units, split as
 * bits 19:4 and 31:20. Same encoding the MOP and the rotators take, which is how
 * it was confirmed - copying a buffer whose physical address came from a MOP
 * window register worked with the offset applied.
 */
#define DIP_MIU_BASE	0x20000000

static void dip_write_addr(struct mstar_dip *dip, unsigned int lo_reg,
			   unsigned int hi_reg, dma_addr_t addr)
{
	u32 a = (u32)addr - DIP_MIU_BASE;

	dip_update(dip, lo_reg, 0xffff, (a >> 4) & 0xffff);
	dip_update(dip, hi_reg, 0x07ff, a >> 20);
}

/*
 * Only at probe and after a resume. Resetting before a job stops the block doing
 * anything at all, even with the init replayed afterwards.
 */
static void dip_reset(struct mstar_dip *dip)
{
	regmap_update_bits(dip->regmap, DIP_RESET, BIT(0), BIT(0));
	regmap_update_bits(dip->regmap, DIP_RESET, BIT(0), 0);
}

static void dip_hw_init(struct mstar_dip *dip)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dip_init); i++)
		dip_update(dip, dip_init[i].reg, dip_init[i].mask,
			   dip_init[i].val);

	dip_update(dip, DIP_R_TILEREQ, GENMASK(6, 0), DIP_TILEREQ_READ);
	dip_update(dip, DIP_W_CTRL, DIP_W_CTRL_TILEREQ, DIP_TILEREQ_WRITE);

	/* nothing pending, and let it tell us when a frame is done */
	dip_update(dip, DIP_INTR_CLEAR, DIP_INTR_ALL, DIP_INTR_ALL);
	dip_update(dip, DIP_INTR_EN, DIP_INTR_ALL, DIP_INTR_ALL);
}

/*
 * Program one conversion. The order is the vendor's, from
 * MHAL_DIVP_ProcessDramData: the two ports, then the mirror, then the crop, then
 * the geometry, then the colour conversion, and only then the trigger.
 */
static void dip_setup_job(struct mstar_dip_ctx *ctx, dma_addr_t src,
			  dma_addr_t dst)
{
	struct mstar_dip *dip = ctx->dip;
	const struct mstar_dip_fmt *sf = ctx->out.fmt;
	const struct mstar_dip_fmt *df = ctx->cap.fmt;
	static const u8 rfmtsel[] = { 0x00, 0x40, 0x80, 0x80, 0xc0 };
	static const u8 wfmtsel[] = { 0x00, 0x10, 0x20, 0x20, 0x30 };
	bool csc = sf->yuv && !df->yuv;

	/*
	 * The whole init goes in before every job. Driven by hand, a job that had
	 * only been set up - ports, geometry, mirror, trigger - moved nothing at
	 * all unless the init had been replayed immediately beforehand, even
	 * though every register read back with the value it had been given. So
	 * something in here does not survive being left alone, and until it is
	 * known what, replaying the lot is what makes the block work.
	 */
	dip_hw_init(dip);

	/* read port */
	dip_update(dip, DIP_R_FMT, GENMASK(7, 6), rfmtsel[sf->hw]);
	dip_update(dip, DIP_R_HALFWIDTH, GENMASK(11, 0),
		   ctx->out.bytesperline >> 1);
	dip_update(dip, DIP_R_STRIDE0, GENMASK(11, 0), ctx->out.bytesperline);
	dip_update(dip, DIP_R_STRIDE1, GENMASK(11, 0),
		   sf->semiplanar ? ctx->out.bytesperline : 0);
	dip_update(dip, DIP_R_STRIDE2, GENMASK(12, 0), 0);
	dip_write_addr(dip, DIP_R_ADDR0_LO, DIP_R_ADDR0_HI, src);
	dip_update(dip, DIP_R_SEMIPLANAR, BIT(14), sf->semiplanar ? BIT(14) : 0);
	dip_update(dip, DIP_CTRL, DIP_CTRL_SEMIPLANAR,
		   sf->semiplanar ? DIP_CTRL_SEMIPLANAR : 0);
	if (sf->semiplanar) {
		dip_write_addr(dip, DIP_R_ADDR1_LO, DIP_R_ADDR1_HI,
			       src + ctx->out.bytesperline * ctx->out.height);
		dip_update(dip, DIP_R_MISC, GENMASK(10, 9), GENMASK(10, 9));
	}

	/* write port */
	dip_update(dip, DIP_W_FMT, GENMASK(5, 4), wfmtsel[df->hw]);
	dip_update(dip, DIP_W_STRIDE0, GENMASK(11, 0), ctx->cap.bytesperline);
	dip_update(dip, DIP_W_STRIDE1, GENMASK(11, 0),
		   df->semiplanar ? ctx->cap.bytesperline : 0);
	dip_update(dip, DIP_W_STRIDE2, GENMASK(11, 0), 0);
	dip_write_addr(dip, DIP_W_ADDR0_LO, DIP_W_ADDR0_HI, dst);
	dip_update(dip, DIP_W_SEMIPLANAR, BIT(14), df->semiplanar ? BIT(14) : 0);
	if (df->semiplanar)
		dip_write_addr(dip, DIP_W_ADDR1_LO, DIP_W_ADDR1_HI,
			       dst + ctx->cap.bytesperline * ctx->cap.height);
	dip_update(dip, DIP_W_MISC1, BIT(12), df->semiplanar ? BIT(12) : 0);
	dip_update(dip, DIP_W_MISC2, BIT(5), df->semiplanar ? BIT(5) : 0);
	dip_update(dip, DIP_W_CTRL, DIP_W_CTRL_FMT3,
		   df->hw == DIP_FMT_32BIT_ALT ? DIP_W_CTRL_FMT3 : 0);

	/*
	 * Only the horizontal mirror is offered, because only the horizontal one
	 * does anything: the vertical bit reads back set and the output is
	 * unchanged. Reversing interleaved chroma swaps U and V, which is what
	 * the swap bit is for.
	 */
	dip_update(dip, DIP_MIRROR, DIP_MIRROR_H | DIP_MIRROR_V,
		   ctx->hflip ? DIP_MIRROR_H : 0);
	dip_update(dip, DIP_UVSWAP, DIP_UVSWAP_EN,
		   ctx->hflip && sf->yuv ? DIP_UVSWAP_EN : 0);

	/* the whole frame */
	dip_update(dip, DIP_CROP_CTRL, GENMASK(1, 0), DIP_CROP_OFF);

	/* geometry; a 1:1 pass means every ratio register is zero */
	dip_update(dip, DIP_SC_BYPASS, BIT(12), 0);
	dip_update(dip, DIP_SC_MODE, GENMASK(1, 0), 0);
	dip_update(dip, DIP_SC_ENABLE, GENMASK(2, 1), GENMASK(2, 1));
	dip_update(dip, DIP_SC_SRC_W, GENMASK(12, 0), ctx->out.width);
	dip_update(dip, DIP_SC_SRC_H, GENMASK(11, 0), ctx->out.height);
	dip_update(dip, DIP_SC_DST_W, GENMASK(12, 0), ALIGN(ctx->cap.width, 16));
	dip_update(dip, DIP_SC_DST_H, GENMASK(11, 0), ctx->cap.height);
	if (ctx->out.width == ctx->cap.width &&
	    ctx->out.height == ctx->cap.height) {
		dip_update(dip, DIP_SC_HRATIO_A, 0xffff, 0);
		dip_update(dip, DIP_SC_HRATIO_B, 0x01ff, 0);
		dip_update(dip, DIP_SC_VRATIO_A, 0xffff, 0);
		dip_update(dip, DIP_SC_VRATIO_B, 0x01ff, 0);
		dip_update(dip, DIP_SC_RATIO_MISC, GENMASK(3, 0), 0);
	}

	/* YUV in, RGB out needs the conversion on; the other way is untested */
	dip_update(dip, DIP_W_CTRL, DIP_W_CTRL_Y2R, csc ? DIP_W_CTRL_Y2R : 0);
	dip_update(dip, DIP_R2Y, BIT(0), 0);

	/* the vendor pulses this after configuring, then triggers */
	dip_update(dip, DIP_SC_ENABLE, GENMASK(2, 1), BIT(2));
	dip_update(dip, DIP_SC_ENABLE, BIT(0), BIT(0));
	dip_update(dip, DIP_SC_ENABLE, BIT(0), 0);

	/* the done status is cleared before the run, not after */
	regmap_write(dip->regmap, DIP_IDLE, 0);
	dip_update(dip, DIP_CTRL, DIP_CTRL_TRIGGER, DIP_CTRL_TRIGGER);
}

static void dip_finish(struct mstar_dip *dip, enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;
	struct mstar_dip_ctx *ctx;

	spin_lock(&dip->irqlock);
	ctx = dip->curr;
	dip->curr = NULL;
	spin_unlock(&dip->irqlock);

	if (!ctx)
		return;

	cancel_delayed_work(&dip->watchdog);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (src && dst) {
		dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
		dst->flags = src->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
		v4l2_m2m_buf_done(src, state);
		v4l2_m2m_buf_done(dst, state);
	}

	v4l2_m2m_job_finish(dip->m2m_dev, ctx->fh.m2m_ctx);
}

static irqreturn_t mstar_dip_irq(int irq, void *data)
{
	struct mstar_dip *dip = data;
	unsigned int status;

	regmap_read(dip->regmap, DIP_INTR_STATUS, &status);
	if (!(status & DIP_INTR_ALL))
		return IRQ_NONE;

	dip_update(dip, DIP_INTR_CLEAR, DIP_INTR_ALL, DIP_INTR_ALL);

	dip_finish(dip, VB2_BUF_STATE_DONE);

	return IRQ_HANDLED;
}

/*
 * Driven by hand from userspace, without the interrupt, one trigger moved about a
 * line and a frame never finished. If that is still true with the interrupt wired
 * up then a job will land here, and failing it is much better than leaving the
 * queue stuck forever.
 */
static void mstar_dip_watchdog(struct work_struct *work)
{
	struct mstar_dip *dip = container_of(to_delayed_work(work),
					     struct mstar_dip, watchdog);
	unsigned int idle, status;

	regmap_read(dip->regmap, DIP_IDLE, &idle);
	regmap_read(dip->regmap, DIP_INTR_STATUS, &status);
	dev_warn(dip->dev, "job timed out, idle 0x%04x status 0x%04x\n",
		 idle, status);
	{
		unsigned int rl, rh, wl, wh, sw, sh2, ctrl;

		regmap_read(dip->regmap, DIP_R_ADDR0_LO, &rl);
		regmap_read(dip->regmap, DIP_R_ADDR0_HI, &rh);
		regmap_read(dip->regmap, DIP_W_ADDR0_LO, &wl);
		regmap_read(dip->regmap, DIP_W_ADDR0_HI, &wh);
		regmap_read(dip->regmap, DIP_SC_SRC_W, &sw);
		regmap_read(dip->regmap, DIP_SC_SRC_H, &sh2);
		regmap_read(dip->regmap, DIP_CTRL, &ctrl);
		dev_warn(dip->dev,
			 "  read %04x:%04x write %04x:%04x src %ux%u ctrl %04x\n",
			 rh, rl, wh, wl, sw, sh2, ctrl);
	}

	dip_reset(dip);
	dip_hw_init(dip);
	dip_finish(dip, VB2_BUF_STATE_ERROR);
}

static void mstar_dip_device_run(void *priv)
{
	struct mstar_dip_ctx *ctx = priv;
	struct mstar_dip *dip = ctx->dip;
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		v4l2_m2m_job_finish(dip->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	spin_lock(&dip->irqlock);
	dip->curr = ctx;
	spin_unlock(&dip->irqlock);

	schedule_delayed_work(&dip->watchdog,
			      msecs_to_jiffies(DIP_JOB_TIMEOUT_MS));

	dev_dbg(dip->dev, "job: src %pad dst %pad %ux%u -> %ux%u\n",
		&(dma_addr_t){ vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0) },
		&(dma_addr_t){ vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0) },
		ctx->out.width, ctx->out.height, ctx->cap.width, ctx->cap.height);

	dip_setup_job(ctx, vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
		      vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0));
}

static void mstar_dip_job_abort(void *priv)
{
	struct mstar_dip_ctx *ctx = priv;

	dip_finish(ctx->dip, VB2_BUF_STATE_ERROR);
}

static const struct v4l2_m2m_ops mstar_dip_m2m_ops = {
	.device_run = mstar_dip_device_run,
	.job_abort = mstar_dip_job_abort,
};

/* the vidioc ops are called with priv as NULL, so the context comes from file */
static struct mstar_dip_ctx *file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct mstar_dip_ctx, fh);
}

static struct mstar_dip_q_data *dip_q_data(struct mstar_dip_ctx *ctx,
					   enum v4l2_buf_type type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->out : &ctx->cap;
}

static int dip_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct mstar_dip_ctx *ctx = vb2_get_drv_priv(vq);
	struct mstar_dip_q_data *q = dip_q_data(ctx, vq->type);

	if (*nplanes) {
		if (*nplanes != 1 || sizes[0] < q->sizeimage)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = q->sizeimage;

	return 0;
}

static int dip_buf_prepare(struct vb2_buffer *vb)
{
	struct mstar_dip_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct mstar_dip_q_data *q = dip_q_data(ctx, vb->vb2_queue->type);

	if (vb2_plane_size(vb, 0) < q->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, q->sizeimage);

	return 0;
}

static void dip_buf_queue(struct vb2_buffer *vb)
{
	struct mstar_dip_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void dip_stop_streaming(struct vb2_queue *vq)
{
	struct mstar_dip_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops mstar_dip_qops = {
	.queue_setup = dip_queue_setup,
	.buf_prepare = dip_buf_prepare,
	.buf_queue = dip_buf_queue,
	.stop_streaming = dip_stop_streaming,
};

static int dip_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct mstar_dip_ctx *ctx = priv;
	struct mstar_dip *dip = ctx->dip;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->ops = &mstar_dip_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &dip->mutex;
	src->dev = dip->dev;

	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &mstar_dip_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &dip->mutex;
	dst->dev = dip->dev;

	return vb2_queue_init(dst);
}

static int dip_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mstar_dip_ctx *ctx = container_of(ctrl->handler,
						 struct mstar_dip_ctx,
						 ctrl_handler);

	if (ctrl->id == V4L2_CID_HFLIP) {
		ctx->hflip = ctrl->val;
		return 0;
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops mstar_dip_ctrl_ops = {
	.s_ctrl = dip_s_ctrl,
};

static int dip_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MStar DIP", sizeof(cap->card));

	return 0;
}

static int dip_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(dip_formats))
		return -EINVAL;

	f->pixelformat = dip_formats[f->index].fourcc;

	return 0;
}

static int dip_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mstar_dip_ctx *ctx = file2ctx(file);
	struct mstar_dip_q_data *q = dip_q_data(ctx, f->type);

	f->fmt.pix.pixelformat = q->fmt->fourcc;
	f->fmt.pix.width = q->width;
	f->fmt.pix.height = q->height;
	f->fmt.pix.bytesperline = q->bytesperline;
	f->fmt.pix.sizeimage = q->sizeimage;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int dip_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	const struct mstar_dip_fmt *fmt;

	fmt = dip_find_fmt(f->fmt.pix.pixelformat);
	if (!fmt) {
		fmt = &dip_formats[0];
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	/*
	 * The geometry registers are thirteen and twelve bits, and chroma
	 * subsampling wants both even.
	 */
	f->fmt.pix.width = clamp_t(u32, ALIGN(f->fmt.pix.width, 2), 16, 4080);
	f->fmt.pix.height = clamp_t(u32, ALIGN(f->fmt.pix.height, 2), 16, 4095);
	while (!dip_width_ok(f->fmt.pix.width))
		f->fmt.pix.width += 2;
	f->fmt.pix.bytesperline = ALIGN(f->fmt.pix.width * fmt->bpp, 16);
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	if (fmt->semiplanar)
		f->fmt.pix.sizeimage += f->fmt.pix.bytesperline *
					f->fmt.pix.height / 2;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int dip_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mstar_dip_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	int ret;

	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = dip_try_fmt(file, priv, f);
	if (ret)
		return ret;

	dip_q_data_set_fmt(dip_q_data(ctx, f->type),
			   dip_find_fmt(f->fmt.pix.pixelformat),
			   f->fmt.pix.width, f->fmt.pix.height);

	return 0;
}

static const struct v4l2_ioctl_ops mstar_dip_ioctl_ops = {
	.vidioc_querycap = dip_querycap,

	.vidioc_enum_fmt_vid_cap = dip_enum_fmt,
	.vidioc_enum_fmt_vid_out = dip_enum_fmt,
	.vidioc_g_fmt_vid_cap = dip_g_fmt,
	.vidioc_g_fmt_vid_out = dip_g_fmt,
	.vidioc_try_fmt_vid_cap = dip_try_fmt,
	.vidioc_try_fmt_vid_out = dip_try_fmt,
	.vidioc_s_fmt_vid_cap = dip_s_fmt,
	.vidioc_s_fmt_vid_out = dip_s_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int mstar_dip_open(struct file *file)
{
	struct mstar_dip *dip = video_drvdata(file);
	struct mstar_dip_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dip = dip;
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 1);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &mstar_dip_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	ret = ctx->ctrl_handler.error;
	if (ret)
		goto err_ctrls;
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	dip_q_data_set_fmt(&ctx->out, &dip_formats[0], 640, 480);
	dip_q_data_set_fmt(&ctx->cap, &dip_formats[0], 640, 480);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dip->m2m_dev, ctx, dip_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}

	v4l2_fh_add(&ctx->fh, file);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return ret;
}

static int mstar_dip_release(struct file *file)
{
	struct mstar_dip_ctx *ctx = file2ctx(file);

	v4l2_fh_del(&ctx->fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations mstar_dip_fops = {
	.owner = THIS_MODULE,
	.open = mstar_dip_open,
	.release = mstar_dip_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct video_device mstar_dip_videodev = {
	.name = DRIVER_NAME,
	.vfl_dir = VFL_DIR_M2M,
	.fops = &mstar_dip_fops,
	.ioctl_ops = &mstar_dip_ioctl_ops,
	.minor = -1,
	.release = video_device_release_empty,
	.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct regmap_config mstar_dip_regmap_config = {
	.name = DRIVER_NAME,
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_dip_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_dip *dip;
	void __iomem *base;
	int irq, ret;

	dip = devm_kzalloc(dev, sizeof(*dip), GFP_KERNEL);
	if (!dip)
		return -ENOMEM;

	dip->dev = dev;
	spin_lock_init(&dip->irqlock);
	mutex_init(&dip->mutex);
	INIT_DELAYED_WORK(&dip->watchdog, mstar_dip_watchdog);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	dip->regmap = devm_regmap_init_mmio(dev, base, &mstar_dip_regmap_config);
	if (IS_ERR(dip->regmap))
		return PTR_ERR(dip->regmap);

	/*
	 * Without this the block looks half broken rather than unclocked: the
	 * mirror and chroma swap registers latch writes, and everything to do
	 * with geometry reads back as zero.
	 */
	dip->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(dip->clk))
		return dev_err_probe(dev, PTR_ERR(dip->clk),
				     "Failed to get the DIP clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, mstar_dip_irq, 0, dev_name(dev), dip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to claim the interrupt\n");

	dip_reset(dip);
	dip_hw_init(dip);

	ret = v4l2_device_register(dev, &dip->v4l2_dev);
	if (ret)
		return ret;

	dip->m2m_dev = v4l2_m2m_init(&mstar_dip_m2m_ops);
	if (IS_ERR(dip->m2m_dev)) {
		ret = PTR_ERR(dip->m2m_dev);
		goto err_v4l2;
	}

	dip->vfd = mstar_dip_videodev;
	dip->vfd.lock = &dip->mutex;
	dip->vfd.v4l2_dev = &dip->v4l2_dev;
	video_set_drvdata(&dip->vfd, dip);

	ret = video_register_device(&dip->vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;

	platform_set_drvdata(pdev, dip);
	dev_info(dev, "MStar DIP as /dev/video%d\n", dip->vfd.num);

	return 0;

err_m2m:
	v4l2_m2m_release(dip->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dip->v4l2_dev);

	return ret;
}

static void mstar_dip_remove(struct platform_device *pdev)
{
	struct mstar_dip *dip = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&dip->watchdog);
	video_unregister_device(&dip->vfd);
	v4l2_m2m_release(dip->m2m_dev);
	v4l2_device_unregister(&dip->v4l2_dev);
}

/* the sleep reset takes the whole block down, so put it back together */
static int mstar_dip_resume(struct device *dev)
{
	struct mstar_dip *dip = dev_get_drvdata(dev);

	dip_reset(dip);
	dip_hw_init(dip);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mstar_dip_pm_ops, NULL, mstar_dip_resume);

static const struct of_device_id mstar_dip_of_match[] = {
	{ .compatible = "sstar,ssd20xd-dip" },
	{ }
};
MODULE_DEVICE_TABLE(of, mstar_dip_of_match);

static struct platform_driver mstar_dip_driver = {
	.probe = mstar_dip_probe,
	.remove = mstar_dip_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_dip_of_match,
		.pm = pm_sleep_ptr(&mstar_dip_pm_ops),
	},
};
module_platform_driver(mstar_dip_driver);

MODULE_DESCRIPTION("SigmaStar SSD20xD DIP image processor");
MODULE_AUTHOR("Daniel's intern <intern@thingy.jp>");
MODULE_LICENSE("GPL");
