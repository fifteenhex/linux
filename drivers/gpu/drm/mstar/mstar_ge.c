// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_fourcc.h>
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <uapi/drm/mstar_ge.h>

#define DRIVER_NAME "mstar-ge"

/* Runtime-PM autosuspend delay: hold the engine awake this long after the
 * queue drains so a burst of ops doesn't bounce resume/suspend per job.
 */
#define MSTAR_GE_AUTOSUSPEND_MS 50

#define REG_CTRL		0x0
#define REG_CTRL1		0x4
#define REG_CMQ_STATUS		0x1c
/*
 * Source colour key thresholds - datasheet GE_SCK_HTH (102828h..10282Bh) and
 * GE_SCK_LTH (10282Ch..10282Fh). Per the datasheet the raw pixel value is
 * only used for the palette/index modes ("[7:0] 8-bit Palette, [15:0] 16-bit
 * Index Mode"); true-colour sources are compared in the engine's internal
 * ARGB8888 domain ("[31:0] ARGB8888"). The vendor driver (mhal.ko
 * GFX_ConvertRGB2DBFmt) therefore expands an RGB565 key to a 565-quantised,
 * bit-replicated ARGB8888 value before writing these registers; ARGB8888
 * keys are written raw. For an exact-match key the high and low threshold
 * hold the same value; low <= pixel <= high is a match (per channel).
 */
#define REG_SCK_HTH_L		0x50
#define REG_SCK_HTH_H		0x54
#define REG_SCK_LTH_L		0x58
#define REG_SCK_LTH_H		0x5c
/* Colour/alpha key operation modes - datasheet 102838h */
#define REG_KEYS_MODE		0x70
#define REG_IRQ			0x78
#define REG_SRCL		0x80
#define REG_SRCH		0x84
#define REG_DSTL		0x98
#define REG_DSTH		0x9c
/* This seems to be for descriptors in DRAM */
#define REG_VCMQ_BASE		0xa0
#define REG_BLENDFLAGS		0xac
#define REG_CONST		0xb0
#define REG_P256_GB		0xb4
#define REG_P256_AR		0xb8
#define REG_P256_INDEX		0xbc
#define REG_SRCPITCH		0xc0
#define REG_TAGL		0xc4
#define REG_TAGH		0xc8
#define REG_DSTPITCH		0xcc
#define REG_COLORFMT		0xd0
#define REG_I0_C		0xd4
#define REG_I1_C		0xdc
#define REG_CLIP_LEFT		0x154
#define REG_CLIP_RIGHT		0x158
#define REG_CLIP_TOP		0x15c
#define REG_CLIP_BOTTOM		0x160
#define REG_ROT			0x164
#define REG_STBB_INI_DX		0x178
#define REG_STBB_INI_DY		0x17c
#define REG_CMD			0x180
#define REG_LINE_CTRL0		0x184
#define REG_LINE_CTRL1		0x188
#define REG_LINE_LENGTH		0x18c
#define REG_STBB_DX		0x190
#define REG_STBB_DY		0x194
#define REG_X0			0x1a0
#define REG_Y0			0x1a4
#define REG_X1			0x1a8
#define REG_Y1			0x1ac
#define REG_X2			0x1b0
#define REG_Y2			0x1b4
#define REG_BITBLT_SRCWIDTH	0x1b8
#define REG_BITBLT_SRCHEIGHT	0x1bc
#define REG_BG_ST		0x1c0
#define REG_RA_ST		0x1c4
/*
 * Gradient fill colour deltas - datasheet GE_PRI_{R,G,B}_D{X,Y}
 * (1028E4h..1028FBh), 20 bit two's complement s7.12 split into a 16 bit low
 * register and a 4 bit high register, and GE_PRI_A_D{X,Y}
 * (1028FCh..1028FFh), 16 bit two's complement s4.11.
 */
#define REG_R_DX_L		0x1c8
#define REG_R_DX_H		0x1cc
#define REG_R_DY_L		0x1d0
#define REG_R_DY_H		0x1d4
#define REG_G_DX_L		0x1d8
#define REG_G_DX_H		0x1dc
#define REG_G_DY_L		0x1e0
#define REG_G_DY_H		0x1e4
#define REG_B_DX_L		0x1e8
#define REG_B_DX_H		0x1ec
#define REG_B_DY_L		0x1f0
#define REG_B_DY_H		0x1f4
#define REG_A_DX		0x1f8
#define REG_A_DY		0x1fc

static const struct reg_field en_field = REG_FIELD(REG_CTRL, 0, 0);
static const struct reg_field abl_field = REG_FIELD(REG_CTRL, 2, 2);
/* EN_GE_SCK - source colour key enable */
static const struct reg_field sck_en_field = REG_FIELD(REG_CTRL, 6, 6);
static const struct reg_field dfb_field = REG_FIELD(REG_CTRL, 10, 10);
/*
 * "Calculate source width/height". Undocumented in the MST786 datasheet
 * (102801h bits 7:5 are marked reserved there) but used by the vendor
 * driver: mhal.ko HAL_GE_EnableCalcSrc_WidthHeight() sets this bit for
 * every rotated (90/180/270) bitblt and clears it for rotation 0 and for
 * real stretches. Presumably it makes the engine derive the source fetch
 * extent from the rotated destination walk. Set for rotated blits, cleared
 * at the start of every job.
 */
static const struct reg_field calc_srcwh_field = REG_FIELD(REG_CTRL, 13, 13);

static const struct reg_field clk_en_field = REG_FIELD(REG_CTRL1, 15, 15);

static const struct reg_field gebusy_field = REG_FIELD(REG_CMQ_STATUS, 0, 0);
static const struct reg_field cmq_free_field = REG_FIELD(REG_CMQ_STATUS, 3, 7);
static const struct reg_field cmq2_free_field = REG_FIELD(REG_CMQ_STATUS, 11, 15);

/* source colour key thresholds */
static const struct reg_field sck_hth_l_field = REG_FIELD(REG_SCK_HTH_L, 0, 15);
static const struct reg_field sck_hth_h_field = REG_FIELD(REG_SCK_HTH_H, 0, 15);
static const struct reg_field sck_lth_l_field = REG_FIELD(REG_SCK_LTH_L, 0, 15);
static const struct reg_field sck_lth_h_field = REG_FIELD(REG_SCK_LTH_H, 0, 15);
/*
 * GE_SCK_OP_MODE:
 * 0 - a source pixel that matches the key keeps the destination pixel
 * 1 - a source pixel that does not match the key keeps the destination pixel
 */
static const struct reg_field sck_op_mode_field = REG_FIELD(REG_KEYS_MODE, 0, 0);

static const struct reg_field irq_mask_field = REG_FIELD(REG_IRQ, 6, 7);
static const struct reg_field irq_force_field = REG_FIELD(REG_IRQ, 8, 9);
static const struct reg_field irq_clr_field = REG_FIELD(REG_IRQ, 10, 11);
static const struct reg_field irq_status_field = REG_FIELD(REG_IRQ, 12, 13);

/* src buffer */
static const struct reg_field srcl_field = REG_FIELD(REG_SRCL, 0, 15);
static const struct reg_field srch_field = REG_FIELD(REG_SRCH, 0, 12);

/* dst buffer */
static const struct reg_field dstl_field = REG_FIELD(REG_DSTL, 0, 15);
static const struct reg_field dsth_field = REG_FIELD(REG_DSTH, 0, 12);

/* blend flags */
static const struct reg_field bld_coloralpha_field	= REG_FIELD(REG_BLENDFLAGS, 0, 0);
static const struct reg_field bld_alphachan_field	= REG_FIELD(REG_BLENDFLAGS, 1, 1);
static const struct reg_field bld_colorize_field	= REG_FIELD(REG_BLENDFLAGS, 2, 2);
static const struct reg_field bld_srcpremul_field	= REG_FIELD(REG_BLENDFLAGS, 3, 3);
static const struct reg_field bld_srcpremulcol_field	= REG_FIELD(REG_BLENDFLAGS, 4, 4);
static const struct reg_field bld_dstpremul_field	= REG_FIELD(REG_BLENDFLAGS, 5, 5);
static const struct reg_field bld_xor_field		= REG_FIELD(REG_BLENDFLAGS, 6, 6);
static const struct reg_field bld_demultiply_field	= REG_FIELD(REG_BLENDFLAGS, 7, 7);

/* const */
static const struct reg_field const_b_field		= REG_FIELD(REG_BLENDFLAGS, 8, 15);
static const struct reg_field const_g_field		= REG_FIELD(REG_CONST, 0, 8);
static const struct reg_field const_r_field		= REG_FIELD(REG_CONST, 8, 15);

static const struct reg_field srcpitch_field = REG_FIELD(REG_SRCPITCH, 0, 13);
static const struct reg_field dstpitch_field = REG_FIELD(REG_DSTPITCH, 0, 13);

static const struct reg_field src_colorfmt_field = REG_FIELD(REG_COLORFMT, 0, 4);
static const struct reg_field dst_colorfmt_field = REG_FIELD(REG_COLORFMT, 8, 12);
#define COLOR_FORMAT_RGB565	0x8
#define COLOR_FORMAT_ARGB8888	0xf

static const struct reg_field clip_left_field = REG_FIELD(REG_CLIP_LEFT, 0, 11);
static const struct reg_field clip_right_field = REG_FIELD(REG_CLIP_RIGHT, 0, 11);
static const struct reg_field clip_top_field = REG_FIELD(REG_CLIP_TOP, 0, 11);
static const struct reg_field clip_bottom_field = REG_FIELD(REG_CLIP_BOTTOM, 0, 11);

static const struct reg_field rot_field = REG_FIELD(REG_ROT, 0, 1);

static const struct reg_field prim_type_field = REG_FIELD(REG_CMD, 4, 6);
#define PRIM_TYPE_LINE		1
#define PRIM_TYPE_RECTFILL	3
#define PRIM_TYPE_BITBLT	4
#define PRIM_TYPE_FREEZE	7

static const struct reg_field pri_s_y_dir_field = REG_FIELD(REG_CMD, 8, 8);
static const struct reg_field pri_x_dir_field = REG_FIELD(REG_CMD, 9, 9);
static const struct reg_field pri_y_dir_field = REG_FIELD(REG_CMD, 10, 10);
/* GE_RECT_CH/GE_RECT_CV - 0: constant colour fill, 1: gradient colour fill */
static const struct reg_field rect_ch_field = REG_FIELD(REG_CMD, 12, 12);
static const struct reg_field rect_cv_field = REG_FIELD(REG_CMD, 13, 13);
/* GE_STBB_TYPE - 0: bilinear, 1: nearest */
static const struct reg_field stbb_type_field = REG_FIELD(REG_CMD, 14, 14);

/* s1.12 */
static const struct reg_field line_delta_field = REG_FIELD(REG_LINE_CTRL0, 1, 14);
static const struct reg_field line_major_field = REG_FIELD(REG_LINE_CTRL0, 15, 15);
static const struct reg_field line_ptn_field = REG_FIELD(REG_LINE_CTRL1, 0, 5);
static const struct reg_field line_ptnrpf_field = REG_FIELD(REG_LINE_CTRL1, 6, 7);
static const struct reg_field line_ptnrst_field = REG_FIELD(REG_LINE_CTRL1, 8, 8);
static const struct reg_field line_last_field = REG_FIELD(REG_LINE_CTRL1, 9, 9);
static const struct reg_field line_length_field = REG_FIELD(REG_LINE_LENGTH, 0, 11);

static const struct reg_field x0_field = REG_FIELD(REG_X0, 0, 11);
static const struct reg_field y0_field = REG_FIELD(REG_Y0, 0, 11);
static const struct reg_field x1_field = REG_FIELD(REG_X1, 0, 11);
static const struct reg_field y1_field = REG_FIELD(REG_Y1, 0, 11);
static const struct reg_field x2_field = REG_FIELD(REG_X2, 0, 11);
static const struct reg_field y2_field = REG_FIELD(REG_Y2, 0, 11);

static const struct reg_field b_st_field = REG_FIELD(REG_BG_ST, 0, 7);
static const struct reg_field g_st_field = REG_FIELD(REG_BG_ST, 8, 15);
static const struct reg_field r_st_field = REG_FIELD(REG_RA_ST, 0, 7);
static const struct reg_field a_st_field = REG_FIELD(REG_RA_ST, 8, 15);

static const struct reg_field tagh_field = REG_FIELD(REG_TAGL, 0, 15);
static const struct reg_field tagl_field = REG_FIELD(REG_TAGH, 0, 15);

static const struct reg_field p256_b_field = REG_FIELD(REG_P256_GB, 0, 7);
static const struct reg_field p256_g_field = REG_FIELD(REG_P256_GB, 8, 15);
static const struct reg_field p256_r_field = REG_FIELD(REG_P256_AR, 0, 7);
static const struct reg_field p256_a_field = REG_FIELD(REG_P256_AR, 8, 15);
static const struct reg_field p256_index_field = REG_FIELD(REG_P256_INDEX, 0, 7);
static const struct reg_field p256_rw_field = REG_FIELD(REG_P256_INDEX, 8, 8);

/* stretch blit */
static const struct reg_field stbb_en_field = REG_FIELD(REG_CTRL1, 4, 4);
static const struct reg_field stbb_ini_dx_field = REG_FIELD(REG_STBB_INI_DX, 0, 12);
static const struct reg_field stbb_ini_dy_field = REG_FIELD(REG_STBB_INI_DY, 0, 12);
static const struct reg_field stbb_dx_field = REG_FIELD(REG_STBB_DX, 0, 12);
static const struct reg_field stbb_dy_field = REG_FIELD(REG_STBB_DY, 0, 12);
static const struct reg_field bitblt_srcwidth_field = REG_FIELD(REG_BITBLT_SRCWIDTH, 0, 11);
static const struct reg_field bitblt_srcheight_field = REG_FIELD(REG_BITBLT_SRCHEIGHT, 0, 11);

/*
 * Compiled job representation.
 *
 * All of the per-op derivation (format conversion, rotation vertex
 * convention, colour-key expansion, gradient deltas, stretch factors, flip
 * direction bits, clip window, buffer addresses/pitches) runs ONCE and is
 * captured as a compact sequence of masked register writes. Starting the job
 * then just replays that sequence to the engine - nothing is re-derived on
 * the submit hot path, and a program can be stored and fired many times
 * (MSTAR_GE_IOCTL_COMPILE_JOB / FIRE_JOB).
 *
 * Each entry is applied with regmap_update_bits(), so bits outside the mask
 * keep whatever value the engine currently has - exactly like the
 * regmap_field_write() calls this replaces. That preserves the partial-update
 * semantics jobs rely on (e.g. a STRBLT without MSTAR_GE_BLIT_NO_BLEND
 * deliberately inherits the previous blit's blend state) and keeps the
 * regcache coherent for the timeout-recovery resync.
 *
 * Writes to the CMD register are collected separately in cmd_mask/cmd_val
 * and applied LAST with a forced write, because writing the primitive type
 * fires the engine. This replaces the old regcache_cache_only() tricks used
 * for the fields that share the CMD register with prim_type.
 */
struct mstar_ge_prog_write {
	u16 reg;
	u16 mask;
	u16 val;
};

/*
 * The busiest ops today (colour-keyed bitblt, gradient fill) touch around 30
 * distinct registers; leave some headroom.
 */
#define MSTAR_GE_PROG_MAX_WRITES 40

struct mstar_ge_prog {
	u8 op;			/* enum mstar_ge_op, for debug logging */
	u8 num_writes;
	/* CMD register bits, applied last: writing prim_type fires the job */
	u16 cmd_mask;
	u16 cmd_val;
	struct mstar_ge_prog_write writes[MSTAR_GE_PROG_MAX_WRITES];
};

/* Emit a masked register write into a program, merging same-register writes */
static void mstar_ge_prog_emit(struct mstar_ge_prog *prog, unsigned int reg,
			       u16 mask, u16 val)
{
	int i;

	val &= mask;

	if (reg == REG_CMD) {
		prog->cmd_mask |= mask;
		prog->cmd_val = (prog->cmd_val & ~mask) | val;
		return;
	}

	for (i = 0; i < prog->num_writes; i++) {
		if (prog->writes[i].reg == reg) {
			prog->writes[i].mask |= mask;
			prog->writes[i].val = (prog->writes[i].val & ~mask) | val;
			return;
		}
	}

	if (WARN_ON(prog->num_writes >= MSTAR_GE_PROG_MAX_WRITES))
		return;

	prog->writes[prog->num_writes].reg = reg;
	prog->writes[prog->num_writes].mask = mask;
	prog->writes[prog->num_writes].val = val;
	prog->num_writes++;
}

/* The compile-time analogue of regmap_field_write() */
static void mstar_ge_prog_field(struct mstar_ge_prog *prog,
				const struct reg_field *field,
				unsigned int val)
{
	u16 mask = GENMASK(field->msb, field->lsb);

	mstar_ge_prog_emit(prog, field->reg, mask, val << field->lsb);
}

struct mstar_ge {
	struct device *dev;
	struct drm_device *drm_device;
	struct clk *clk;
	u32 tag;

	struct regmap *regmap;

	struct regmap_field *en, *abl, *dfb, *clk_en, *busy;
	struct regmap_field *calc_srcwh;
	struct regmap_field *cmq_free, *cmq2_free;
	struct regmap_field *irq_mask, *irq_force, *irq_clr, *irq_status;
	struct regmap_field *srcl, *srch;
	struct regmap_field *dstl, *dsth;

	struct regmap_field *bld_alphachan;

	/* source colour key */
	struct regmap_field *sck_en, *sck_op_mode;
	struct regmap_field *sck_hth_l, *sck_hth_h, *sck_lth_l, *sck_lth_h;

	/* gradient fill */
	struct regmap_field *rect_ch, *rect_cv;

	struct regmap_field *srcpitch, *dstpitch;
	struct regmap_field *srcclrfmt, *dstclrfmt;
	struct regmap_field *clip_left, *clip_right, *clip_top, *clip_bottom;
	struct regmap_field *rot;
	struct regmap_field *prim_type, *pri_s_y_dir, *pri_x_dir, *pri_y_dir;
	struct regmap_field *line_delta, *line_major, *line_last, *line_length;
	struct regmap_field *x0, *y0, *x1, *y1, *x2, *y2;

	struct regmap_field *tagl, *tagh;

	/* stretch blit */
	struct regmap_field *stbb_en;
	struct regmap_field *stbb_type;
	struct regmap_field *stbb_ini_dx, *stbb_ini_dy;
	struct regmap_field *stbb_dx, *stbb_dy;
	struct regmap_field *bitblt_src_width, *bitblt_src_height;

	/* p256 */
	struct regmap_field *p256_b, *p256_g, *p256_r, *p256_a, *p256_index, *p256_rw;

	struct regmap_field *b_st, *g_st, *r_st, *a_st;

	spinlock_t lock;
	struct list_head queue;
	int inflight;
	int cur_op;			/* op of the job currently on the engine (debug) */
	wait_queue_head_t dma_wait;

	struct miscdevice ge_dev;

	struct kmem_cache *jobs;

#if defined(CONFIG_PM_DEVFREQ)
	/* devfreq */
	struct devfreq_dev_profile profile;
	struct devfreq *devfreq;
#endif
};

#define mstar_ge_buf_sz(b) (b->cfg.pitch * b->cfg.height)

/*
 * Debug aid for job timeouts. Toggle at runtime:
 *   echo 1 > /sys/module/mstar_ge/parameters/debug
 * With debug on, every job's op is logged as it is submitted; a job timeout
 * always dumps the GE register state (regardless of the flag) so the failing
 * op can be identified.
 */
static int mstar_ge_debug;
module_param_named(debug, mstar_ge_debug, int, 0644);
MODULE_PARM_DESC(debug, "log every GE op (job timeouts always dump registers)");

#define GE_DBG(ge, fmt, ...) \
	do { if (mstar_ge_debug) dev_info((ge)->dev, fmt, ##__VA_ARGS__); } while (0)

static const char *mstar_ge_opname(int op)
{
	switch (op) {
	case MSTAR_GE_OP_LINE:			return "LINE";
	case MSTAR_GE_OP_RECTFILL:		return "RECTFILL";
	case MSTAR_GE_OP_RECTFILL_GRADIENT:	return "RECTFILL_GRADIENT";
	case MSTAR_GE_OP_BITBLT:		return "BITBLT";
	case MSTAR_GE_OP_STRBLT:		return "STRBLT";
	default:				return "?";
	}
}

static bool mstar_ge_volatile_reg(struct device *dev, unsigned int reg)
{
	//printk("%s:%x\n", __func__, reg);

	switch(reg) {
	case REG_IRQ:
	//case REG_CMD:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mstar_ge_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
	/*
	 * Using the cache works around the fact that
	 * some registers cannot be written and readback.
	 * This allows fields to work properly.
	 */
	.volatile_reg = mstar_ge_volatile_reg,
	.max_register = 0x200,
	/*
	 * The interrupt handler reads and writes registers through this
	 * regmap, so it must be a spinlock (fast_io), not the default mutex,
	 * and the cache must not allocate under that lock: a flat cache is
	 * allocated once at init, the rbtree cache allocates on first write.
	 */
	.fast_io = true,
	.cache_type = REGCACHE_FLAT,
};

struct mstar_ge_dma_buf {
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *dma_attachment;
};

/* Everything needed to do a single operation */
struct mstar_ge_job {
	unsigned long tag;

	struct mstar_ge_opdata opdata;

	/* src and dst buffer handling */
	struct mstar_ge_buf_cfg src_cfg, dst_cfg;
	dma_addr_t src_addr, dst_addr;

	/* DMA_BUF stuff */
	struct mstar_ge_dma_buf src_dma_buf, dst_dma_buf;

	/* compiled register program, replayed to start the job */
	struct mstar_ge_prog prog;

	struct list_head queue;

	bool dma_done;
};

static inline void mstar_ge_job_init(void *job)
{
	struct mstar_ge_job *ge_job = job;

	INIT_LIST_HEAD(&ge_job->queue);
}

/*
 * A persistently mapped buffer (ABI v2). The dma-buf is attached and mapped
 * once at registration time and the resulting device address is reused by
 * every QUEUE2 op that references the handle, so the hot path pays no dma-buf
 * attach/map/unmap/detach cost. Mapped DMA_BIDIRECTIONAL because a registered
 * surface can be both a blit source and a destination over its lifetime.
 */
struct mstar_ge_reg_buf {
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	size_t size;
	int refcount;
};

/*
 * A pre-compiled job (ABI v3): a batch of register programs compiled once by
 * MSTAR_GE_IOCTL_COMPILE_JOB and replayed by MSTAR_GE_IOCTL_FIRE_JOB. It pins
 * the registered buffers whose device addresses are baked into the programs
 * (one mstar_ge_reg_buf reference each) so the mappings outlive any handle
 * the owner might unregister.
 */
struct mstar_ge_cjob {
	/* registered buffer handles this job holds a reference on (0 = none) */
	u32 src_handle, dst_handle;
	/* protected by mstar_ge_file.lock */
	int refcount;
	unsigned int num_progs;
	struct mstar_ge_prog progs[];
};

/*
 * Per-open-file state. Registered buffer handles and compiled-job tokens live
 * here so they are scoped to the file description and cleaned up on close,
 * and so two independent openers cannot see each other's handles/tokens.
 */
struct mstar_ge_file {
	struct mstar_ge *ge;
	struct idr buf_idr;	/* handle -> struct mstar_ge_reg_buf */
	struct idr cjob_idr;	/* token -> struct mstar_ge_cjob */
	unsigned int num_cjobs;	/* live tokens, capped at MSTAR_GE_MAX_COMPILED_JOBS */
	struct mutex lock;	/* protects buf_idr, cjob_idr, num_cjobs, refcounts */
};

static int mstar_ge_cmq_free(struct mstar_ge *ge)
{
	unsigned int tmp, cmqfree;

	regmap_field_read(ge->cmq_free, &cmqfree);
	regmap_field_read(ge->cmq2_free, &tmp);
	cmqfree += tmp;

	return cmqfree;
}

static void mstar_ge_asciiart(void *bm, int width, int height)
{
	int y, x;
	unsigned int a, r, g, b;

	for(y = 0; y < height; y++){
		void *line = ((u16*) bm) + (width * y);
		printk("%16ph\n", line);
		//for (x = 0; x < width; x++) {
		//	u16 *p = ((u16*) bm) + (width * y) + x;
		//	pr_cont("%02x\n", (unsigned) *p);
		//}

	}
}

static void mstar_ge_tag(struct mstar_ge *ge)
{
	ge->tag++;
	regmap_field_force_write(ge->tagl, ge->tag);
	regmap_field_force_write(ge->tagh, ge->tag >> 16);
	dev_dbg(ge->dev, "tag is: %d\n", (unsigned) ge->tag);
}

static void mstar_ge_set_src(struct mstar_ge_prog *prog, dma_addr_t dmaaddr, unsigned int pitch)
{
	mstar_ge_prog_field(prog, &srcl_field, dmaaddr);
	mstar_ge_prog_field(prog, &srch_field, dmaaddr >> 16);
	mstar_ge_prog_field(prog, &srcpitch_field, pitch);
}

static void mstar_ge_set_dst(struct mstar_ge_prog *prog, dma_addr_t dmaaddr, unsigned int pitch)
{
	mstar_ge_prog_field(prog, &dstl_field, dmaaddr);
	mstar_ge_prog_field(prog, &dsth_field, dmaaddr >> 16);
	mstar_ge_prog_field(prog, &dstpitch_field, pitch);
}

static void mstar_ge_set_clip(struct mstar_ge_prog *prog, unsigned int left, unsigned int top,
		unsigned int right, unsigned int bottom)
{
	mstar_ge_prog_field(prog, &clip_left_field, left);
	mstar_ge_prog_field(prog, &clip_top_field, top);
	mstar_ge_prog_field(prog, &clip_right_field, right);
	mstar_ge_prog_field(prog, &clip_bottom_field, bottom);
}

static void mstar_ge_set_priv0(struct mstar_ge_prog *prog, unsigned int x, unsigned int y)
{
	mstar_ge_prog_field(prog, &x0_field, x);
	mstar_ge_prog_field(prog, &y0_field, y);
}

static void mstar_ge_set_priv1(struct mstar_ge_prog *prog, unsigned int x, unsigned int y)
{
	mstar_ge_prog_field(prog, &x1_field, x);
	mstar_ge_prog_field(prog, &y1_field, y);
}

static void mstar_ge_set_priv2(struct mstar_ge_prog *prog, unsigned int x, unsigned int y)
{
	mstar_ge_prog_field(prog, &x2_field, x);
	mstar_ge_prog_field(prog, &y2_field, y);
}

static void mstar_ge_set_start_color(struct mstar_ge_prog *prog,
				      const struct mstar_ge_color *start_color)
{
	mstar_ge_prog_field(prog, &b_st_field, start_color->b);
	mstar_ge_prog_field(prog, &g_st_field, start_color->g);
	mstar_ge_prog_field(prog, &r_st_field, start_color->r);
	mstar_ge_prog_field(prog, &a_st_field, start_color->a);
}

static int mstar_ge_do_line(struct mstar_ge *ge,
			    struct mstar_ge_prog *prog,
			    unsigned int x0,
			    unsigned int y0,
			    unsigned int x1,
			    unsigned int y1)
{

	unsigned int w = max(x0, x1) - min(x0, x1);
	unsigned int h = max(y0, y1) - min(y0, y1);
	bool ymajor = h > w;
	unsigned int length = (ymajor ? h : w) + 1;
	const unsigned int factor = 0x1000;
	/*
	 * delta is the minor-axis fractional step. A horizontal line has h == 0
	 * (and a single point w == h == 0); guard the division so those don't
	 * fault with a kernel divide error - the minor step is simply 0 then.
	 */
	int delta = h ? (w * factor) / h : 0;

	//if (ymajor) {
	//	if (y0 > y1)
	//		delta = -delta;
	//}
	if (x0 > x1)
		mstar_ge_prog_field(prog, &pri_x_dir_field, 1);

	dev_dbg(ge->dev, "compiling line draw from %d,%d to %d,%d (area %d x %d, length %d, delta %x, ymajor %d)\n",
			x0, y0, x1, y1,
			w, h, length, delta, ymajor);

	mstar_ge_prog_field(prog, &line_last_field, 1);
	mstar_ge_prog_field(prog, &line_length_field, length);
	mstar_ge_prog_field(prog, &line_major_field, ymajor ? 1 : 0);
	mstar_ge_prog_field(prog, &line_delta_field, delta);

	mstar_ge_set_priv0(prog, x0, y0);
	mstar_ge_set_priv1(prog, x1, y1);

	/* Do it! (the CMD write is applied last, when the program is fired) */
	mstar_ge_prog_field(prog, &prim_type_field, PRIM_TYPE_LINE);

	return 0;
}

/*
 * Gradient colour delta: the per-pixel-step increment from start to end over
 * steps pixel steps, as a two's complement fixed point value with frac_bits
 * fractional bits (s7.12 for R/G/B, s4.11 for A). The division truncates
 * towards zero (C division semantics).
 */
static s32 mstar_ge_gradient_delta(int start, int end, unsigned int steps,
				   unsigned int frac_bits)
{
	if (!steps)
		return 0;

	return (s32) div_s64((s64)(end - start) << frac_bits, steps);
}

/*
 * Write a 20 bit two's complement gradient delta split over a 16 bit low
 * register and a 4 bit high register.
 */
static void mstar_ge_write_delta20(struct mstar_ge_prog *prog, unsigned int reg_l,
				   unsigned int reg_h, s32 delta)
{
	u32 v = ((u32) delta) & 0xfffff;

	mstar_ge_prog_emit(prog, reg_l, 0xffff, v & 0xffff);
	mstar_ge_prog_emit(prog, reg_h, 0xffff, v >> 16);
}

static int mstar_ge_do_rectfill(struct mstar_ge *ge,
				struct mstar_ge_prog *prog,
			        unsigned int left,
				unsigned int top,
				unsigned int right,
				unsigned int bottom)
{
	dev_dbg(ge->dev, "compiling rect fill, %d %d -> %d %d\n",
			left, top, right, bottom);

	mstar_ge_set_priv0(prog, left, top);
	mstar_ge_set_priv1(prog, right, bottom);

	mstar_ge_prog_field(prog, &prim_type_field, PRIM_TYPE_RECTFILL);

	return 0;
}

static int mstar_ge_do_rectfill_gradient(struct mstar_ge *ge,
			struct mstar_ge_prog *prog,
			const struct mstar_ge_rectfill_gradient_params *p,
			unsigned int left,
			unsigned int top,
			unsigned int right,
			unsigned int bottom)
{
	bool grad_h = p->flags & MSTAR_GE_RECTFILL_GRADIENT_H;
	bool grad_v = p->flags & MSTAR_GE_RECTFILL_GRADIENT_V;
	int sa = (p->start_argb >> 24) & 0xff;
	int sr = (p->start_argb >> 16) & 0xff;
	int sg = (p->start_argb >> 8) & 0xff;
	int sb = p->start_argb & 0xff;
	int ea = (p->end_argb >> 24) & 0xff;
	int er = (p->end_argb >> 16) & 0xff;
	int eg = (p->end_argb >> 8) & 0xff;
	int eb = p->end_argb & 0xff;
	struct mstar_ge_color start_color = {
		.a = sa, .r = sr, .g = sg, .b = sb,
	};
	/*
	 * The colour walks from the start colour at the first pixel of an
	 * axis to the end colour at the last one, so there are (last - first)
	 * steps on each axis (coordinates are inclusive). An axis without
	 * gradient gets a zero delta.
	 */
	unsigned int xsteps = grad_h ? right - left : 0;
	unsigned int ysteps = grad_v ? bottom - top : 0;

	dev_dbg(ge->dev, "compiling gradient rect fill, %d %d -> %d %d (flags 0x%x, %08x -> %08x)\n",
			left, top, right, bottom, p->flags,
			p->start_argb, p->end_argb);

	mstar_ge_set_start_color(prog, &start_color);

	mstar_ge_write_delta20(prog, REG_R_DX_L, REG_R_DX_H,
		mstar_ge_gradient_delta(sr, er, xsteps, 12));
	mstar_ge_write_delta20(prog, REG_G_DX_L, REG_G_DX_H,
		mstar_ge_gradient_delta(sg, eg, xsteps, 12));
	mstar_ge_write_delta20(prog, REG_B_DX_L, REG_B_DX_H,
		mstar_ge_gradient_delta(sb, eb, xsteps, 12));
	mstar_ge_prog_emit(prog, REG_A_DX, 0xffff,
		((u32) mstar_ge_gradient_delta(sa, ea, xsteps, 11)) & 0xffff);

	mstar_ge_write_delta20(prog, REG_R_DY_L, REG_R_DY_H,
		mstar_ge_gradient_delta(sr, er, ysteps, 12));
	mstar_ge_write_delta20(prog, REG_G_DY_L, REG_G_DY_H,
		mstar_ge_gradient_delta(sg, eg, ysteps, 12));
	mstar_ge_write_delta20(prog, REG_B_DY_L, REG_B_DY_H,
		mstar_ge_gradient_delta(sb, eb, ysteps, 12));
	mstar_ge_prog_emit(prog, REG_A_DY, 0xffff,
		((u32) mstar_ge_gradient_delta(sa, ea, ysteps, 11)) & 0xffff);

	/*
	 * The gradient enables share the CMD register with prim_type, so they
	 * land in cmd_mask/cmd_val and are flushed by the single CMD write
	 * that fires the engine.
	 */
	mstar_ge_prog_field(prog, &rect_ch_field, grad_h ? 1 : 0);
	mstar_ge_prog_field(prog, &rect_cv_field, grad_v ? 1 : 0);

	mstar_ge_set_priv0(prog, left, top);
	mstar_ge_set_priv1(prog, right, bottom);

	mstar_ge_prog_field(prog, &prim_type_field, PRIM_TYPE_RECTFILL);

	return 0;
}

static int mstar_ge_drm_color_to_gop(u32 fourcc)
{
	switch(fourcc) {
	// need to ignore the alpha for this?
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return COLOR_FORMAT_ARGB8888;
	case DRM_FORMAT_RGB565:
		return COLOR_FORMAT_RGB565;
	};

	return -ENOTSUPP;
}

/* Datasheet limits for the vertex registers (GE_PRI_Vn_X/Y, 1028D0h..) */
#define MSTAR_GE_MAX_X	4079
#define MSTAR_GE_MAX_Y	4095

static int mstar_ge_do_bitblt(struct mstar_ge *ge,
			      struct mstar_ge_prog *prog,
			      const struct mstar_ge_bitblt *bitblt,
			      const struct mstar_ge_buf_cfg *src_cfg,
			      const struct mstar_ge_buf_cfg *dst_cfg)
{
	/* For source flipping */
	unsigned int src_x0 = bitblt->src_x0;
	unsigned int dst_w = bitblt->dst_x1 - bitblt->dst_x0;
	unsigned int src_y0 = bitblt->src_y0;
	unsigned int dst_h = bitblt->dst_y1 - bitblt->dst_y0;
	/* Source extent programmed into GE_STBB_S_W/H */
	unsigned int src_w = src_cfg->width;
	unsigned int src_h = src_cfg->height;
	/* For dest flipping/rotation */
	unsigned int dst_x0;
	unsigned int dst_x1;
	unsigned int dst_y0;
	unsigned int dst_y1;
	unsigned int rot;

	dev_dbg(ge->dev, "compiling bitblt %d, %d -> %d:%d,%d:%d\n",
			bitblt->src_x0, bitblt->src_y0,
			bitblt->dst_x0, bitblt->dst_y0,
			bitblt->dst_x1, bitblt->dst_y1);

	mstar_ge_prog_field(prog, &dfb_field, 1);
	/*
	 * Blend the source over the destination using the source alpha channel,
	 * unless the caller asked for a plain opaque copy. Blending an opaque
	 * ARGB/XRGB source (alpha != 0xff) would corrupt the copy, so an opaque
	 * blit must clear this.
	 */
	mstar_ge_prog_field(prog, &bld_alphachan_field,
			    bitblt->flags & MSTAR_GE_BLIT_NO_BLEND ? 0 : 1);

	/*
	 * Source colour key: source pixels that exactly match the key (a raw
	 * source-format pixel value; RGB565 in [15:0], ARGB8888 in [31:0])
	 * leave the destination pixel untouched. sck_en was cleared for every
	 * job in mstar_ge_run_job().
	 *
	 * The hardware compares true-colour sources in its internal ARGB8888
	 * domain (GE_SCK_HTH, 102828h: the raw value only applies to the
	 * palette/index modes). An RGB565 source pixel is expanded to 8888
	 * before the compare, so a raw 565 key in the threshold registers
	 * never matches anything - the key has to be expanded too. The vendor
	 * driver (mhal.ko GFX_ConvertRGB2DBFmt) writes the 565-quantised,
	 * bit-replicated 8888 value; we program the whole quantisation bucket
	 * (low threshold = channel bits zero-filled, high threshold =
	 * one-filled, alpha 0x00..0xff) so the exact low-bit expansion and
	 * the alpha the engine assigns to 565 pixels don't matter. Distinct
	 * 565 colours can never fall into the same bucket. Assumes the
	 * threshold compare is per channel - flagged for HW validation.
	 */
	if (bitblt->flags & MSTAR_GE_BLIT_SRC_COLORKEY) {
		u32 lth = bitblt->colorkey;
		u32 hth = bitblt->colorkey;

		if (src_cfg->fourcc == DRM_FORMAT_RGB565) {
			u32 key = bitblt->colorkey & 0xffff;
			u32 rgb = ((key & 0xf800) << 8) |	/* R5 -> [23:19] */
				  ((key & 0x07e0) << 5) |	/* G6 -> [15:10] */
				  ((key & 0x001f) << 3);	/* B5 -> [7:3] */

			lth = rgb;
			hth = 0xff000000 | rgb | 0x00070307;
		}

		mstar_ge_prog_field(prog, &sck_hth_l_field, hth & 0xffff);
		mstar_ge_prog_field(prog, &sck_hth_h_field, hth >> 16);
		mstar_ge_prog_field(prog, &sck_lth_l_field, lth & 0xffff);
		mstar_ge_prog_field(prog, &sck_lth_h_field, lth >> 16);
		mstar_ge_prog_field(prog, &sck_op_mode_field, 0);
		mstar_ge_prog_field(prog, &sck_en_field, 1);
	}

	mstar_ge_prog_field(prog, &stbb_en_field, 0);

	/* set the region to copy to */

	/*
	 * Compute the destination vertices per rotation into the locals below;
	 * the unconditional set_priv0/set_priv1 (after the optional flip swap)
	 * programs them.
	 *
	 * For a rotated blit the engine does NOT take the destination
	 * footprint corners. Per the vendor driver (mhal.ko MDrv_GE_BitBltEX,
	 * the only silicon-validated reference - the MST786 datasheet only
	 * documents GE_ROT itself, 1028B2h [1:0]):
	 *  - (X0, Y0) is where source pixel (0,0) lands in the destination,
	 *  - (X1, Y1) = (X0 + copy_w - 1, Y0 + copy_h - 1), i.e. a source-
	 *    sized walk rect anchored at X0/Y0 (X1/Y1 may legitimately exceed
	 *    the footprint/surface for 90/180/270),
	 *  - GE_STBB_S_W/H hold the copied rect's width/height,
	 *  - CTRL bit13 ("calc src width/height") must be set.
	 * copy_w x copy_h is the source-side size of the copied block; for
	 * 90/270 the destination footprint is the same block transposed, so
	 * copy_w = footprint height and copy_h = footprint width.
	 *
	 * The uapi dst rectangle is always the destination footprint (the
	 * pixels that end up written), inclusive corners. The previous code
	 * programmed made-up vertices (e.g. X1 = dst_x1 * 2 - 1) that don't
	 * match the source extent and hang the engine (CMQ_STATUS busy
	 * forever).
	 */
	switch (bitblt->flags & MSTAR_GE_ROTATION_MASK) {
	case MSTAR_GE_ROTATION_0:
		dst_x0 = bitblt->dst_x0;
		dst_y0 = bitblt->dst_y0;
		dst_x1 = bitblt->dst_x1;
		dst_y1 = bitblt->dst_y1;
		rot = 0;
		break;
	case MSTAR_GE_ROTATION_90:
	case MSTAR_GE_ROTATION_180:
	case MSTAR_GE_ROTATION_270:
		/*
		 * Combining destination/source flips with a real rotation is
		 * untested against the vendor convention (the vendor driver
		 * folds them together in a different order); reject the
		 * combination instead of hanging the engine with vertices
		 * that don't match the walk.
		 */
		if (bitblt->flags & (MSTAR_GE_FLIP_SRC_V |
				     MSTAR_GE_FLIP_DST_H |
				     MSTAR_GE_FLIP_DST_V))
			return -EINVAL;

		/*
		 * The rotated footprint must be an ordered rectangle inside
		 * the destination surface: unlike rotation 0 there is no
		 * safety net here (the clip window can't save us from a
		 * walk/source-extent mismatch), and an out-of-range rectangle
		 * hangs the engine rather than getting clipped.
		 */
		if (bitblt->dst_x1 < bitblt->dst_x0 ||
		    bitblt->dst_y1 < bitblt->dst_y0 ||
		    bitblt->dst_x1 >= dst_cfg->width ||
		    bitblt->dst_y1 >= dst_cfg->height)
			return -EINVAL;

		if ((bitblt->flags & MSTAR_GE_ROTATION_MASK) ==
					MSTAR_GE_ROTATION_90) {
			/* src (0,0) lands on the footprint's top right */
			dst_x0 = bitblt->dst_x1;
			dst_y0 = bitblt->dst_y0;
			dst_x1 = bitblt->dst_x1 + dst_h;
			dst_y1 = bitblt->dst_y0 + dst_w;
			src_w = dst_h + 1;
			src_h = dst_w + 1;
			rot = 1;
		} else if ((bitblt->flags & MSTAR_GE_ROTATION_MASK) ==
					MSTAR_GE_ROTATION_180) {
			/* src (0,0) lands on the footprint's bottom right */
			dst_x0 = bitblt->dst_x1;
			dst_y0 = bitblt->dst_y1;
			dst_x1 = bitblt->dst_x1 + dst_w;
			dst_y1 = bitblt->dst_y1 + dst_h;
			src_w = dst_w + 1;
			src_h = dst_h + 1;
			rot = 2;
		} else {
			/* 270: src (0,0) lands on the footprint's bottom left */
			dst_x0 = bitblt->dst_x0;
			dst_y0 = bitblt->dst_y1;
			dst_x1 = bitblt->dst_x0 + dst_h;
			dst_y1 = bitblt->dst_y1 + dst_w;
			src_w = dst_h + 1;
			src_h = dst_w + 1;
			rot = 3;
		}

		/*
		 * The computed anchor + walk extent must fit the 12 bit
		 * vertex registers (X limit 4079, Y limit 4095 per the
		 * datasheet, 1028D0h..1028D7h); a truncated write would
		 * again program a bogus rectangle.
		 */
		if (dst_x1 > MSTAR_GE_MAX_X || dst_y1 > MSTAR_GE_MAX_Y)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	mstar_ge_prog_field(prog, &bitblt_srcwidth_field, src_w);
	mstar_ge_prog_field(prog, &bitblt_srcheight_field, src_h);

	mstar_ge_prog_field(prog, &rot_field, rot);
	/* vendor: HAL_GE_EnableCalcSrc_WidthHeight(rot != 0) */
	mstar_ge_prog_field(prog, &calc_srcwh_field, rot ? 1 : 0);

	/* the direction bits share the CMD register - flushed by the fire write */
	if (bitblt->flags & MSTAR_GE_FLIP_DST_H) {
		mstar_ge_prog_field(prog, &pri_x_dir_field, 1);
		swap(dst_x0, dst_x1);
	}
	if (bitblt->flags & MSTAR_GE_FLIP_DST_V) {
		mstar_ge_prog_field(prog, &pri_y_dir_field, 1);
		swap(dst_y0, dst_y1);
	}
	if (bitblt->flags & MSTAR_GE_FLIP_SRC_V) {
		mstar_ge_prog_field(prog, &pri_s_y_dir_field, 1);
		/* with vertical src flipping the start point is the bottom corner */
		src_y0 += dst_h;
	}

	/* set the destination area */
	mstar_ge_set_priv0(prog, dst_x0, dst_y0);
	mstar_ge_set_priv1(prog, dst_x1, dst_y1);

	/* set the start corner of the source */
	mstar_ge_set_priv2(prog, src_x0, src_y0);

	mstar_ge_prog_field(prog, &prim_type_field, PRIM_TYPE_BITBLT);

	return 0;
}

static int mstar_ge_do_strblt(struct mstar_ge *ge,
		struct mstar_ge_prog *prog, unsigned int width,
		unsigned int height, const struct mstar_ge_strblt *strblt)
{
	int srcw = strblt->src_x1 - strblt->src_x0;
	int dstw = strblt->dst_x1 - strblt->dst_x0;
	int srch = strblt->src_y1 - strblt->src_y0;
	int dsth = strblt->dst_y1 - strblt->dst_y0;
	int rot;

	dev_dbg(ge->dev, "compiling strblt %d:%d,%d:%d -> %d:%d,%d:%d\n",
			strblt->src_x0, strblt->src_y0,
			strblt->src_x1, strblt->src_y1,
			strblt->dst_x0, strblt->dst_y0,
			strblt->dst_x1, strblt->dst_y1);


	int frac = 0x1000;

	/*
	 * A zero-width or zero-height destination rectangle has nothing to
	 * scale into and would divide by zero in the scale-factor maths below,
	 * faulting the kernel. Reject it (the coordinates come straight from the
	 * QUEUE ioctl and are otherwise unvalidated).
	 */
	if (dstw == 0 || dsth == 0)
		return -EINVAL;

	int inidx = ((srcw - dstw) * frac) / (dstw * 2);
	int inidy = ((srch - dsth) * frac) / (dsth * 2);
	int dx = (srcw * frac) / dstw;
	int dy = (srch * frac) / dsth;

	dev_dbg(ge->dev, "src %d x %d, dst %d x %d ini %d (%08x), %d (%08x), d %d (%08x) %d (%08x)",
		srcw, srch, dstw, dsth, inidx, inidx, inidy, inidy, dx, dx, dy, dy);

	/*
	 * Historically STRBLT never touched the blend configuration and so
	 * inherited whatever the previous BITBLT programmed. Opting in to
	 * MSTAR_GE_BLIT_NO_BLEND makes the stretch an opaque copy regardless
	 * of what ran before; without the flag the old behaviour is kept.
	 */
	if (strblt->flags & MSTAR_GE_BLIT_NO_BLEND) {
		mstar_ge_prog_field(prog, &dfb_field, 1);
		mstar_ge_prog_field(prog, &bld_alphachan_field, 0);
	}

	mstar_ge_prog_field(prog, &stbb_ini_dx_field, inidx);
	mstar_ge_prog_field(prog, &stbb_ini_dy_field, inidy);
	mstar_ge_prog_field(prog, &stbb_dx_field, dx);
	mstar_ge_prog_field(prog, &stbb_dy_field, dy);
	mstar_ge_prog_field(prog, &stbb_en_field, 1);

	/*
	 * Scaling filter, GE_STBB_TYPE: 0 = bilinear (the hardware default,
	 * and what this driver always used before the flag existed),
	 * 1 = nearest. Shares the CMD register with prim_type so it lands in
	 * cmd_mask/cmd_val and is flushed by the CMD write that fires.
	 */
	mstar_ge_prog_field(prog, &stbb_type_field,
			    strblt->flags & MSTAR_GE_STRBLT_NEAREST ? 1 : 0);

	mstar_ge_prog_field(prog, &bitblt_srcwidth_field, width);
	mstar_ge_prog_field(prog, &bitblt_srcheight_field, height);


	/* set the region to copy to */
	switch(strblt->flags & MSTAR_GE_ROTATION_MASK) {
		case MSTAR_GE_ROTATION_0:
			mstar_ge_set_priv0(prog, strblt->dst_x0, strblt->dst_y0);
			mstar_ge_set_priv1(prog, strblt->dst_x1, strblt->dst_y1);
			rot = 0;
			break;
		default:
			return -EINVAL;
	}

	mstar_ge_prog_field(prog, &rot_field, rot);

	/* set the top left corner of the source */
	mstar_ge_set_priv2(prog, strblt->src_x0, strblt->src_y0);

	mstar_ge_prog_field(prog, &prim_type_field, PRIM_TYPE_BITBLT);

	return 0;
}

#define P256_ENTRIES 0xff

static void mstar_ge_write_p256(struct mstar_ge *ge)
{
	int i;

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_write(ge->p256_index, i);
		regmap_field_write(ge->p256_r, 0xff);
		regmap_field_write(ge->p256_g, 0xaa);
		regmap_field_write(ge->p256_b, 0x55);
		regmap_field_write(ge->p256_a, 0xff);
		regmap_field_force_write(ge->p256_rw, 1);
	}
}

static void mstar_ge_read_p256(struct mstar_ge *ge)
{
	unsigned r, g, b, a;
	int i;

	regmap_field_write(ge->p256_rw, 0);

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_force_write(ge->p256_index, i);
		regmap_field_read(ge->p256_r, &r);
		regmap_field_read(ge->p256_g, &g);
		regmap_field_read(ge->p256_b, &b);
		regmap_field_read(ge->p256_a, &a);
		dev_warn(ge->dev, "p256 read %x, r: %x, g: %x, b: %x, a: %x\n",
				i, r, g, b, a);
	}
}

/*
 * Compile one op into a register program: run all of the derivation
 * (format lookup, vertex/rotation maths, colour-key expansion, gradient
 * deltas, stretch factors, clip window) once and capture the resulting
 * masked register writes. Does not touch the hardware. src_addr == 0 means
 * "no source buffer" (fill/line ops); src_cfg is then unused.
 */
static int mstar_ge_compile_op(struct mstar_ge *ge,
			       const struct mstar_ge_opdata *opdata,
			       dma_addr_t src_addr,
			       const struct mstar_ge_buf_cfg *src_cfg,
			       dma_addr_t dst_addr,
			       const struct mstar_ge_buf_cfg *dst_cfg,
			       struct mstar_ge_prog *prog)
{
	int dst_fmt = mstar_ge_drm_color_to_gop(dst_cfg->fourcc);
	int src_fmt;

	memset(prog, 0, sizeof(*prog));
	prog->op = opdata->op;

	/* dst is required */
	if (!dst_addr)
		return -EINVAL;

	if (dst_fmt < 0)
		return dst_fmt;

	/* src is optional for some ops*/
	if (src_addr) {
		src_fmt = mstar_ge_drm_color_to_gop(src_cfg->fourcc);
		if (src_fmt < 0)
			return src_fmt;

		dev_dbg(ge->dev, "Setting source %d x %d (%d)\n",
				src_cfg->width, src_cfg->height, src_cfg->pitch);
		mstar_ge_set_src(prog, src_addr, src_cfg->pitch);
		mstar_ge_prog_field(prog, &src_colorfmt_field, src_fmt);
	} else {
		switch (opdata->op) {
		case MSTAR_GE_OP_BITBLT:
		case MSTAR_GE_OP_STRBLT:
			dev_err(ge->dev, "Blit operations require two buffers\n");
			return -EINVAL;
		default:
			break;
		}
	}

	dev_dbg(ge->dev, "Setting destination %d x %d (%d)\n",
			dst_cfg->width, dst_cfg->height, dst_cfg->pitch);
	mstar_ge_set_dst(prog, dst_addr, dst_cfg->pitch);
	mstar_ge_prog_field(prog, &dst_colorfmt_field, dst_fmt);

	mstar_ge_prog_field(prog, &en_field, 1);

	/*
	 * Reset the shared fields the previous job might have set. The
	 * direction/gradient/filter bits share the CMD register so they end
	 * up in cmd_mask; the op emits below overwrite these defaults where
	 * needed (mstar_ge_prog_emit merges same-register writes).
	 */
	mstar_ge_prog_field(prog, &pri_s_y_dir_field, 0);
	mstar_ge_prog_field(prog, &pri_x_dir_field, 0);
	mstar_ge_prog_field(prog, &pri_y_dir_field, 0);
	/* reset the op-specific CMD bits the last job might have set */
	mstar_ge_prog_field(prog, &rect_ch_field, 0);
	mstar_ge_prog_field(prog, &rect_cv_field, 0);
	mstar_ge_prog_field(prog, &stbb_type_field, 0);

	mstar_ge_prog_field(prog, &rot_field, 0);
	/* only rotated bitblts set this (vendor: EnableCalcSrc_WidthHeight) */
	mstar_ge_prog_field(prog, &calc_srcwh_field, 0);

	/* colour keying is opt-in per job */
	mstar_ge_prog_field(prog, &sck_en_field, 0);

	/* set the clip */
	mstar_ge_set_clip(prog, 0, 0,
			dst_cfg->width - 1,
			dst_cfg->height - 1);

	switch (opdata->op) {
	case MSTAR_GE_OP_LINE:
		mstar_ge_set_start_color(prog, &opdata->line.start_color);
		return mstar_ge_do_line(ge, prog,
				 opdata->line.x0,
				 opdata->line.y0,
				 opdata->line.x1,
				 opdata->line.y1);
	case MSTAR_GE_OP_RECTFILL:
		mstar_ge_set_start_color(prog, &opdata->rectfill.start_color);
		return mstar_ge_do_rectfill(ge, prog,
				     opdata->rectfill.x0,
				     opdata->rectfill.y0,
				     min(opdata->rectfill.x1, dst_cfg->width - 1),
				     min(opdata->rectfill.y1, dst_cfg->height - 1));
	case MSTAR_GE_OP_RECTFILL_GRADIENT:
		return mstar_ge_do_rectfill_gradient(ge, prog,
				     &opdata->rectfill_gradient,
				     opdata->rectfill_gradient.x0,
				     opdata->rectfill_gradient.y0,
				     min(opdata->rectfill_gradient.x1, dst_cfg->width - 1),
				     min(opdata->rectfill_gradient.y1, dst_cfg->height - 1));
	case MSTAR_GE_OP_BITBLT:
		return mstar_ge_do_bitblt(ge, prog, &opdata->bitblt,
					  src_cfg, dst_cfg);
	case MSTAR_GE_OP_STRBLT:
		return mstar_ge_do_strblt(ge, prog,
				   src_cfg->width,
				   src_cfg->height,
				   &opdata->strblt);
	default:
		return -EINVAL;
	}
}

/* Compile a job descriptor's op into its embedded register program */
static int mstar_ge_compile_job(struct mstar_ge *ge, struct mstar_ge_job *job)
{
	return mstar_ge_compile_op(ge, &job->opdata,
				   job->src_addr, &job->src_cfg,
				   job->dst_addr, &job->dst_cfg,
				   &job->prog);
}

/*
 * Push a compiled job to the engine: replay its register program and fire.
 * This is the whole submit hot path - no per-op derivation happens here.
 *
 * Called with ge->lock held, and from the completion interrupt when one job's
 * completion starts the next, so nothing here may sleep. In particular the
 * engine is not resumed here: every queued job holds a runtime PM reference
 * taken by mstar_ge_queue_job(), which runs in the submitter's context where
 * sleeping is allowed.
 */
static void mstar_ge_start_job(struct mstar_ge *ge, struct mstar_ge_job *job)
{
	const struct mstar_ge_prog *prog = &job->prog;
	int i;

	mstar_ge_tag(ge);

	ge->cur_op = prog->op;
	GE_DBG(ge, "run op %s (%d), %d register writes, cmq free %d\n",
	       mstar_ge_opname(prog->op), prog->op, prog->num_writes,
	       mstar_ge_cmq_free(ge));

	/*
	 * Replay the program. regmap_update_bits() keeps the bits outside
	 * each write's mask (and the regcache stays coherent for the
	 * timeout-recovery resync). The CMD write goes last and is forced
	 * because writing the primitive type is what fires the engine.
	 */
	for (i = 0; i < prog->num_writes; i++)
		regmap_update_bits(ge->regmap, prog->writes[i].reg,
				   prog->writes[i].mask, prog->writes[i].val);

	regmap_write_bits(ge->regmap, REG_CMD, prog->cmd_mask, prog->cmd_val);

}

/* Dump the GE register state so a stuck/failing job can be diagnosed. */
static void mstar_ge_dump_regs(const struct mstar_ge *ge)
{
	static const struct { const char *name; unsigned int reg; } r[] = {
		{ "CTRL", REG_CTRL }, { "CTRL1", REG_CTRL1 },
		{ "CMQ_STATUS", REG_CMQ_STATUS }, { "IRQ", REG_IRQ },
		{ "SRCL", REG_SRCL }, { "SRCH", REG_SRCH },
		{ "DSTL", REG_DSTL }, { "DSTH", REG_DSTH },
		{ "SRCPITCH", REG_SRCPITCH }, { "DSTPITCH", REG_DSTPITCH },
		{ "BLENDFLAGS", REG_BLENDFLAGS }, { "COLORFMT", REG_COLORFMT },
		{ "ROT", REG_ROT }, { "CMD", REG_CMD },
		{ "X0", REG_X0 }, { "Y0", REG_Y0 }, { "X1", REG_X1 }, { "Y1", REG_Y1 },
		{ "X2", REG_X2 }, { "Y2", REG_Y2 },
		{ "SRCW", REG_BITBLT_SRCWIDTH }, { "SRCH2", REG_BITBLT_SRCHEIGHT },
		{ "STBB_INI_DX", REG_STBB_INI_DX }, { "STBB_INI_DY", REG_STBB_INI_DY },
		{ "STBB_DX", REG_STBB_DX }, { "STBB_DY", REG_STBB_DY },
		{ "SCK_HTH_L", REG_SCK_HTH_L }, { "SCK_HTH_H", REG_SCK_HTH_H },
		{ "SCK_LTH_L", REG_SCK_LTH_L }, { "SCK_LTH_H", REG_SCK_LTH_H },
		{ "KEYS_MODE", REG_KEYS_MODE },
	};
	unsigned int v;
	int i;

	for (i = 0; i < ARRAY_SIZE(r); i++) {
		if (regmap_read(ge->regmap, r[i].reg, &v) == 0)
			dev_err(ge->dev, "  GE %-12s [0x%03x] = 0x%04x\n",
				r[i].name, r[i].reg, v);
	}
}

/*
 * Recover from a wedged engine after a job timeout. Without this a single
 * bad op leaves CMQ_STATUS busy forever, inflight never drops and every
 * following QUEUE ioctl times out too - the device is dead until reboot.
 *
 * Reset: EN_GE (102800h bit0) is documented as "0: Disable and reset to
 * initial state, 1: Enable", so dropping it is the engine soft-reset. It is
 * left low afterwards; the next job's regmap_field_write(ge->en, 1) releases
 * it. In case the reset also puts the register file back to its defaults the
 * regcache is marked dirty and synced back (skipping REG_CMD - flushing the
 * cached CMD value would write prim_type and fire the engine).
 *
 * Cleanup: every queued job is dropped, inflight forced to 0 and dma_wait
 * woken so concurrent waiters get unstuck. Each of those jobs holds a
 * pm_runtime reference taken when it was queued, and no completion interrupt is
 * coming to release it, so one is dropped here per job. A very late completion
 * IRQ racing this sees inflight == 0 and bails out without touching PM or the
 * queue.
 */
static void mstar_ge_recover(struct mstar_ge *ge)
{
	struct mstar_ge_job *job, *tmp;
	unsigned long flags;
	int dropped = 0;
	int i;

	/* Soft-reset the engine and clear any latched interrupt */
	regmap_field_force_write(ge->en, 0);
	regmap_field_force_write(ge->irq_clr, ~0);
	regmap_field_force_write(ge->irq_clr, 0);

	/* Rewrite the configuration in case the reset cleared it */
	regcache_mark_dirty(ge->regmap);
	regcache_sync_region(ge->regmap, 0, REG_CMD - 4);
	regcache_sync_region(ge->regmap, REG_CMD + 4,
			     mstar_ge_regmap_config.max_register);

	spin_lock_irqsave(&ge->lock, flags);
	list_for_each_entry_safe(job, tmp, &ge->queue, queue) {
		list_del_init(&job->queue);
		dropped++;
	}
	ge->inflight = 0;
	spin_unlock_irqrestore(&ge->lock, flags);

	for (i = 0; i < dropped; i++) {
		pm_runtime_mark_last_busy(ge->dev);
		pm_runtime_put_autosuspend(ge->dev);
	}

	wake_up(&ge->dma_wait);

	dev_err(ge->dev, "engine reset, dropped %d stuck job(s)\n", dropped);
}

static int mstar_ge_wait_for_idle(struct mstar_ge *ge)
{
	unsigned int status = 0, irq = 0;

	if (wait_event_timeout(ge->dma_wait, ge->inflight == 0, HZ * 10))
		return 0;

	regmap_read(ge->regmap, REG_CMQ_STATUS, &status);
	regmap_read(ge->regmap, REG_IRQ, &irq);
	dev_err(ge->dev,
		"timeout: %d job(s) unfinished, last op=%d (%s), CMQ_STATUS=0x%04x (busy=%d) IRQ=0x%04x\n",
		ge->inflight, ge->cur_op, mstar_ge_opname(ge->cur_op),
		status, status & 1, irq);
	mstar_ge_dump_regs(ge);

	mstar_ge_recover(ge);

	return -ETIMEDOUT;
}

/*
 * Queue a compiled job; the caller must have filled job->prog.
 *
 * The engine is resumed here, before the lock is taken, because this runs in the
 * submitter's context and resuming sleeps. Doing it where the job is actually
 * pushed to the engine would mean sleeping with ge->lock held and interrupts
 * off, and in the interrupt handler besides: that is a "BUG: sleeping function
 * called from invalid context", which is what happened the first time anything
 * in Linux used this engine.
 *
 * One reference per queued job, released by that job's completion interrupt or
 * by mstar_ge_recover(). A batch therefore keeps the engine resumed from the
 * first job being queued until the last has retired.
 */
static int mstar_ge_queue_job(struct mstar_ge *ge, struct mstar_ge_job *job)
{
	unsigned long flags;
	int ret;

	ret = pm_runtime_resume_and_get(ge->dev);
	if (ret < 0) {
		dev_err(ge->dev, "runtime resume failed %d\n", ret);
		return ret;
	}

	spin_lock_irqsave(&ge->lock, flags);
	ge->inflight++;
	list_add_tail(&job->queue, &ge->queue);

	/* Start the first job */
	if (ge->inflight == 1)
		mstar_ge_start_job(ge, job);

	spin_unlock_irqrestore(&ge->lock, flags);

	return 0;
}

static irqreturn_t mstar_ge_irq(int irq, void *data)
{
	struct mstar_ge *ge = data;
	struct device *dev = ge->dev;
	struct mstar_ge_job *job;
	unsigned int status;
	unsigned long flags;
	int ret = IRQ_HANDLED;

	regmap_field_read(ge->irq_status, &status);
	regmap_field_write(ge->irq_force, 0);

	/*
	 * To clear the irq the clear bits need to be
	 * set, but apparently the hardware doesn't clear
	 * them so if we don't clear them no more interrupts
	 * happen..
	 */
	regmap_field_force_write(ge->irq_clr, ~0);
	regmap_field_force_write(ge->irq_clr, 0);

	GE_DBG(ge, "interrupt, status=0x%x (op %s)\n", status,
	       mstar_ge_opname(ge->cur_op));

	spin_lock_irqsave(&ge->lock, flags);

	if (!ge->inflight) {
		dev_err(dev, "Interrupt when no jobs queued!\n");
		ret = IRQ_NONE;
		goto out;
	}

	/* free the finished job */
	job = list_first_entry(&ge->queue, struct mstar_ge_job, queue);
	job->dma_done = true;
	list_del(&job->queue);
	ge->inflight--;

	/* Run the next job, if the batch has more */
	if (ge->inflight) {
		job = list_first_entry(&ge->queue, struct mstar_ge_job, queue);
		mstar_ge_start_job(ge, job);
	}

	/*
	 * Release the runtime-PM reference with autosuspend so a burst of jobs
	 * (a DirectFB frame is many ops) keeps the engine resumed instead of
	 * bouncing resume/suspend per job; it only suspends once the queue has
	 * been idle for the autosuspend delay.
	 */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	wake_up(&ge->dma_wait);
out:
	spin_unlock_irqrestore(&ge->lock, flags);

	return ret;
}

static void mstar_ge_filltestbuf(const struct mstar_ge_buf *buf)
{
	u32 i, j, y;
	int bytesperpixel = buf->cfg.pitch / buf->cfg.width;

	for(i = 0; i < buf->cfg.height; i++) {
		void *line = buf->buf + (buf->cfg.pitch * i);
		for (j = 0; j < buf->cfg.width; j++) {
			u8 *pixel = line + (bytesperpixel * j);
			for (y = 0; y < bytesperpixel; y++)
				/* Should give a pattern of ~row, col */
				pixel[y] = y % 2 ? j : ~i;
		}
	}
}

static void mstar_ge_cleartestbuf(const struct mstar_ge_buf *buf)
{
	memset(buf->buf, 0, mstar_ge_buf_sz(buf));
}

static int mstar_ge_test_allocbuffers(struct mstar_ge *ge,
		struct mstar_ge_buf *src,
		struct mstar_ge_buf *dst,
		void **src_alloc,
		void **dst_alloc)
{
	src->cfg.width = 8;
	src->cfg.height = 8;
	src->cfg.pitch = src->cfg.width * 2;
	src->cfg.fourcc = DRM_FORMAT_RGB565;

	memcpy(dst, src, sizeof(*dst));

	*src_alloc = kzalloc(mstar_ge_buf_sz(src) * 3, GFP_KERNEL);
	if (!src->buf)
		return -ENOMEM;

	*dst_alloc = kzalloc(mstar_ge_buf_sz(dst) * 3, GFP_KERNEL);
	if (!dst->buf) {
		kfree(src->buf);
		return -ENOMEM;
	}

	/*
	 * Drawable area in the middle of the buffer so we
	 * can detect dma memory corruption
	 */
	src->buf = *src_alloc + mstar_ge_buf_sz(src);
	dst->buf = *dst_alloc + mstar_ge_buf_sz(dst);

	return 0;
}

static dma_addr_t mstar_ge_map_kalloc(struct mstar_ge *ge,
		const struct mstar_ge_buf *buf,
		enum dma_data_direction dir)
{
	return dma_map_single(ge->dev, buf->buf, mstar_ge_buf_sz(buf), dir);
}

static void mstar_ge_unmap_kalloc(
		const struct mstar_ge *ge,
		dma_addr_t addr,
		const struct mstar_ge_buf *buf,
		enum dma_data_direction dir)
{
	dma_unmap_single(ge->dev, addr, mstar_ge_buf_sz(buf), dir);
}

static int mstar_ge_test_mapbuffers(struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	int ret;

	j->src_addr = mstar_ge_map_kalloc(ge, src, DMA_TO_DEVICE);
	ret = dma_mapping_error(ge->dev, j->src_addr);
	if (ret)
		return ret;

	j->dst_addr = mstar_ge_map_kalloc(ge, dst, DMA_FROM_DEVICE);
	ret = dma_mapping_error(ge->dev, j->dst_addr);
	if (ret)
		goto unmap_src;

	return ret;

unmap_src:
	mstar_ge_unmap_kalloc(ge, j->src_addr, src, DMA_TO_DEVICE);
	return ret;
}

static void mstar_ge_test_unmapbuffers(const struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	mstar_ge_unmap_kalloc(ge, j->src_addr, src, DMA_TO_DEVICE);
	mstar_ge_unmap_kalloc(ge, j->dst_addr, dst, DMA_FROM_DEVICE);
}

static bool mstar_ge_optimize_op(const struct mstar_ge *ge,
				 struct mstar_ge_opdata *op)
{
	/*
	 * Using rotation is slower than flipping, so if the rotation
	 * could be a flip do a flip instead.
	 */
	if (op->op == MSTAR_GE_OP_BITBLT &&
	    (op->bitblt.flags & MSTAR_GE_ROTATION_MASK) == MSTAR_GE_ROTATION_180)
	{
		struct mstar_ge_bitblt *bitblt = &op->bitblt;
		u32 orig_flags = bitblt->flags;
		//u32 dst_x0, dst_y0;
		//u32 dst_x1, dst_y1;

		dev_dbg(ge->dev, "Optimizing 180 rotation to h + v flip\n");

		/* Make sure the area is represented left,top -> right,bottom */
		//dst_x0 = min(bitblt->dst_x0, bitblt->dst_x1);
		//dst_y0 = min(bitblt->dst_y0, bitblt->dst_y1);
		//dst_x1 = max(bitblt->dst_x0, bitblt->dst_x1);
		//dst_y1 = max(bitblt->dst_y0, bitblt->dst_y1);

		//bitblt->dst_x0 = dst_x0;
		//bitblt->dst_y0 = dst_y0;
		//bitblt->dst_x1 = dst_x1;
		//bitblt->dst_y1 = dst_y1;

		/*
		 * Clear the rotation and set the flip. Preserve the opaque-copy
		 * and colour-key flags so an opaque or keyed 180 blit doesn't
		 * silently change behaviour.
		 */
		bitblt->flags = MSTAR_GE_ROTATION_0;
		bitblt->flags |= MSTAR_GE_FLIP_DST_H;
		bitblt->flags |= MSTAR_GE_FLIP_DST_V;
		bitblt->flags |= (orig_flags & (MSTAR_GE_BLIT_NO_BLEND |
						MSTAR_GE_BLIT_SRC_COLORKEY));

		return true;
	}

	return false;
}

static int mstar_ge_validate_op_flags(u32 flags)
{
	if (!(flags & MSTAR_GE_ROTATION_MASK))
		return -EINVAL;

	return 0;
}

static int mstar_ge_validate_op(const struct mstar_ge *ge,
				const struct mstar_ge_opdata *op,
				int number)
{
	/* Check the operation isn't garbage */
	switch (op->op) {
	case MSTAR_GE_OP_LINE:
		dev_dbg(ge->dev, "op %d: LINE\n", number);
		break;
	case MSTAR_GE_OP_RECTFILL:
		dev_dbg(ge->dev, "op %d: RECTFILL, %d:%d -> %d:%d\n",
				number,
				op->rectfill.x0, op->rectfill.y0,
				op->rectfill.x1, op->rectfill.y1);
		break;
	case MSTAR_GE_OP_RECTFILL_GRADIENT:
		dev_dbg(ge->dev, "op %d: RECTFILL_GRADIENT, %d:%d -> %d:%d (flags 0x%x)\n",
				number,
				op->rectfill_gradient.x0, op->rectfill_gradient.y0,
				op->rectfill_gradient.x1, op->rectfill_gradient.y1,
				op->rectfill_gradient.flags);

		/* a gradient fill must interpolate on at least one axis */
		if (!(op->rectfill_gradient.flags & (MSTAR_GE_RECTFILL_GRADIENT_H |
						     MSTAR_GE_RECTFILL_GRADIENT_V)))
			return -EINVAL;
		break;
	case MSTAR_GE_OP_BITBLT:
		dev_dbg(ge->dev, "op %d: BITBLT %d,%d -> %d:%d,%d:%d"
				  "(flags 0x%x, src_v_flip %d, dst_h_flip %d, dst_v_flip %d, rot %d)\n",
				  number,
				  op->bitblt.src_x0, op->bitblt.src_y0,
				  op->bitblt.dst_x0, op->bitblt.dst_y0,
				  op->bitblt.dst_x1, op->bitblt.dst_y1,
				  op->bitblt.flags,
				  op->bitblt.flags & MSTAR_GE_FLIP_SRC_V ? 1 : 0,
				  op->bitblt.flags & MSTAR_GE_FLIP_DST_H ? 1 : 0,
				  op->bitblt.flags & MSTAR_GE_FLIP_DST_V ? 1 : 0,
				  op->bitblt.flags & MSTAR_GE_ROTATION_MASK);

		return mstar_ge_validate_op_flags(op->bitblt.flags);
	case MSTAR_GE_OP_STRBLT:
		dev_dbg(ge->dev, "op %d: STRBLT %d,%d,%d,%d -> %d:%d,%d,%d (rot %d)\n", number,
				  op->strblt.src_x0, op->strblt.src_y0,
				  op->strblt.src_x1, op->strblt.src_y1,
				  op->strblt.dst_x0, op->strblt.dst_y0,
				  op->strblt.dst_x1, op->strblt.dst_y1,
				  op->strblt.flags & MSTAR_GE_ROTATION_MASK);

		return mstar_ge_validate_op_flags(op->strblt.flags);
	default:
		return -EINVAL;
	}

	return 0;
}

static int mstar_ge_test_pretest(struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	int ret;

	mstar_ge_validate_op(ge, &j->opdata, 0);

	dev_info(ge->dev, "src before\n");
	mstar_ge_asciiart(src->buf, src->cfg.width, src->cfg.height);
	dev_info(ge->dev, "dst before\n");
	mstar_ge_asciiart(dst->buf, dst->cfg.width, dst->cfg.height);

	ret = mstar_ge_test_mapbuffers(ge, j, src, dst);
	if (ret)
		return ret;

	/* the buffer addresses are baked into the program, so map first */
	ret = mstar_ge_compile_job(ge, j);
	if (ret)
		mstar_ge_test_unmapbuffers(ge, j, src, dst);

	return ret;
}

static void mstar_ge_test_posttest(
		struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst,
		void *src_alloc, void *dst_alloc)
{
	int i;

	mstar_ge_wait_for_idle(ge);

	mstar_ge_test_unmapbuffers(ge, j, src, dst);

	for (i = 0; i < mstar_ge_buf_sz(dst); i++) {
		u8 *b = ((u8*)dst_alloc) + i;
		BUG_ON(*b);
	}

	for (i = 0; i < mstar_ge_buf_sz(dst); i++) {
		u8 *b = ((u8*)dst_alloc) + i + (mstar_ge_buf_sz(dst) * 2);
		BUG_ON(*b);
	}

	dev_info(ge->dev, "src after job\n");
	mstar_ge_asciiart(src->buf, src->cfg.width, src->cfg.height);
	dev_info(ge->dev, "dst after job\n");
	mstar_ge_asciiart(dst->buf, dst->cfg.width, dst->cfg.height);
}

static struct mstar_ge_job* mstar_ge_alloc_job(struct mstar_ge *ge)
{
	struct mstar_ge_job *j;

	j = kmem_cache_alloc(ge->jobs, GFP_KERNEL);
	if (!j)
		return NULL;

	j->dma_done = false;

	return j;
}

static void mstar_ge_reset_job(struct mstar_ge_job *j)
{
	j->dma_done = false;
}

static int mstar_ge_test(struct mstar_ge *ge)
{
	struct mstar_ge_job *j;
	struct mstar_ge_buf src, dst;
	void *src_alloc, *dst_alloc;
	int ret;

	j = mstar_ge_alloc_job(ge);
	if (!j) {
		dev_err(ge->dev, "Failed to allocate test job\n");
		return -ENOMEM;
	}

	/* Setup the job with the common bits */
	ret = mstar_ge_test_allocbuffers(ge, &src, &dst, &src_alloc, &dst_alloc);
	if (ret) {
		dev_err(ge->dev, "Failed to allocate test buffers\n");
		goto free_job;
	}

	memcpy(&j->src_cfg, &src.cfg, sizeof(j->src_cfg));
	memcpy(&j->dst_cfg, &dst.cfg, sizeof(j->dst_cfg));

	/* Line top,left to bottom,right */
	dev_info(ge->dev, "Test, line - top,left to bottom,right\n");
	j->opdata.op = MSTAR_GE_OP_LINE;
	j->opdata.line.x0 = 0;
	j->opdata.line.y0 = 0;
	j->opdata.line.x1 = dst.cfg.width - 1;
	j->opdata.line.y1 = dst.cfg.height - 1;
	j->opdata.line.start_color.r = 0xff;
	j->opdata.line.start_color.g = 0xff;
	j->opdata.line.start_color.b = 0xff;
	j->opdata.line.start_color.a = 0xff;

	//mstar_get_filltestbuf(j->opdata.dst, j->opdata.dst_height, j->opdata.dst_pitch);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* Line top,right to bottom,left */
	dev_info(ge->dev, "Test, line - top,right to bottom,left\n");
	j->opdata.line.x0 = dst.cfg.width - 1;
	j->opdata.line.y0 = 0;
	j->opdata.line.x1 = 0;
	j->opdata.line.y1 = dst.cfg.height - 1;

	//mstar_get_filltestbuf(j->opdata.dst, j->opdata.dst_height, j->opdata.dst_pitch);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* Line vertical */
	dev_info(ge->dev, "Test, line - top to bottom\n");
	j->opdata.line.x0 = (dst.cfg.width / 2) - 1;
	j->opdata.line.y0 = 0;
	j->opdata.line.x1 = (dst.cfg.width / 2) - 1;
	j->opdata.line.y1 = dst.cfg.height - 1;

	//mstar_get_filltestbuf(j->opdata.dst, j->opdata.dst_height, j->opdata.dst_pitch);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* Line horizontal */
	dev_info(ge->dev, "Test, line - left to right\n");
	j->opdata.line.x0 = 0;
	j->opdata.line.y0 = (dst.cfg.height / 2) - 1;
	j->opdata.line.x1 =




			dst.cfg.width - 1;
	j->opdata.line.y1 = (dst.cfg.height / 2) - 1;

	//mstar_get_filltestbuf(j->opdata.dst, j->opdata.dst_height, j->opdata.dst_pitch);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* Rectfill */
	dev_info(ge->dev, "Test, rect fill\n");
	mstar_ge_reset_job(j);
	j->opdata.op = MSTAR_GE_OP_RECTFILL;
	j->opdata.rectfill.x0 = 1;
	j->opdata.rectfill.y0 = 1;
	j->opdata.rectfill.x1 = j->opdata.rectfill.x0 + 3;
	j->opdata.rectfill.y1 = j->opdata.rectfill.y0 + 3;
	j->opdata.rectfill.start_color.r = 0x55;
	j->opdata.rectfill.start_color.g = 0x00;
	j->opdata.rectfill.start_color.b = 0x00;
	j->opdata.rectfill.start_color.a = 0xff;

	mstar_ge_filltestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 0 rotation */
	dev_info(ge->dev, "Test, bitblt, no rotation \n");
	mstar_ge_reset_job(j);
	j->opdata.op = MSTAR_GE_OP_BITBLT;
	j->opdata.bitblt.src_x0 = 2;
	j->opdata.bitblt.src_y0 = 2;
	j->opdata.bitblt.dst_x0 = 1;
	j->opdata.bitblt.dst_y0 = 1;
	j->opdata.bitblt.dst_x1 = 3;
	j->opdata.bitblt.dst_y1 = 3;
	j->opdata.bitblt.flags = MSTAR_GE_ROTATION_0;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	goto test_strblt;

	/* bitblt, 90 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 90\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.flags = MSTAR_GE_ROTATION_90;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 180 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 180\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.flags = MSTAR_GE_ROTATION_180;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge,j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 270 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 270\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.flags = MSTAR_GE_ROTATION_270;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge,j, &src, &dst, src_alloc, dst_alloc);

	/* strblt */
test_strblt:
	dev_info(ge->dev, "Test, strblt, rotation 0\n");
	mstar_ge_reset_job(j);
	j->opdata.op = MSTAR_GE_OP_STRBLT;
	j->opdata.strblt.src_x0 = 0;
	j->opdata.strblt.src_y0 = 0;
	j->opdata.strblt.src_x1 = dst.cfg.width - 1;
	j->opdata.strblt.src_y1 = dst.cfg.height - 1;
	j->opdata.strblt.dst_x0 = 0;
	j->opdata.strblt.dst_y0 = 0;
	j->opdata.strblt.dst_x1 = (dst.cfg.width / 2) - 1;
	j->opdata.strblt.dst_y1 = (dst.cfg.height / 2) - 1;
	//
	j->opdata.strblt.flags = MSTAR_GE_ROTATION_0;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge,j, &src, &dst, src_alloc, dst_alloc);

	/* clean up */
free_src:
	kfree(src_alloc);
	kfree(dst_alloc);
free_job:
	kmem_cache_free(ge->jobs, j);

	return ret;
}

static int mstar_ge_validate_buf_cfg(struct mstar_ge *ge,
				     const struct mstar_ge_buf_cfg *cfg,
				     int number)
{
	const struct drm_format_info *info;

	if (mstar_ge_drm_color_to_gop(cfg->fourcc) < 0) {
		dev_warn(ge->dev, "Unhandled buffer type: %p4cc\n", &cfg->fourcc);
		return -EINVAL;
	}

	/*
	 * Geometry comes straight from userspace and is programmed into the GE
	 * as the DMA pitch / dimensions, so reject anything degenerate or where
	 * a pixel row would not fit inside the stride (which would let the
	 * engine walk off the end of each row). The whole surface is bounded
	 * against the imported dma-buf size by the caller.
	 */
	info = drm_format_info(cfg->fourcc);
	if (!info || !cfg->width || !cfg->height ||
	    cfg->pitch < (u64)cfg->width * info->cpp[0]) {
		dev_warn(ge->dev, "bad buffer geometry %ux%u pitch %u\n",
			 cfg->width, cfg->height, cfg->pitch);
		return -EINVAL;
	}

	return 0;
}

static int mstar_ge_validate_buffer(struct mstar_ge *ge, struct mstar_ge_buf *buf, int number)
{
	dev_dbg(ge->dev, "buffer: %d, fd %d %dpx x %dpx, pitch %d, format %p4cc\n",
			 number, buf->fd, buf->cfg.width, buf->cfg.height, buf->cfg.pitch,
			 &buf->cfg.fourcc);

	if (buf->fd < 0)
		return -EINVAL;

	return mstar_ge_validate_buf_cfg(ge, &buf->cfg, number);
}

/*
 * Queue a batch of compiled programs back-to-back on the engine and block
 * until the whole batch has retired. Shared by every submit path: QUEUE (v1),
 * QUEUE2 (v2) and FIRE_JOB (v3, pre-compiled programs stored in the kernel).
 *
 * The caller guarantees num_progs is in [1, MSTAR_GE_MAX_JOBS] and that the
 * backing buffers stay mapped for the duration (this call blocks until idle).
 */
static int mstar_ge_run_progs(struct mstar_ge *ge,
			      const struct mstar_ge_prog *progs,
			      unsigned int num_progs)
{
	struct mstar_ge_job *jobs[MSTAR_GE_MAX_JOBS];
	unsigned int i, njobs = 0;
	int ret = 0;
	int idle_ret;

	if (WARN_ON(num_progs < 1 || num_progs > MSTAR_GE_MAX_JOBS))
		return -EINVAL;

	for (i = 0; i < num_progs; i++) {
		struct mstar_ge_job *job;

		job = mstar_ge_alloc_job(ge);
		if (!job) {
			dev_err(ge->dev, "Failed to allocate job descriptor\n");
			ret = -ENOMEM;
			break;
		}

		job->prog = progs[i];
		jobs[njobs++] = job;

		ret = mstar_ge_queue_job(ge, job);
		if (ret)
			break;
	}

	/*
	 * Always wait for whatever was queued before returning - jobs already
	 * on the engine reference jobs[] memory, so they must retire before we
	 * free it even if a later job failed to start. A timeout here resets
	 * the engine and flushes the queue (see mstar_ge_recover()).
	 */
	idle_ret = mstar_ge_wait_for_idle(ge);
	if (!ret)
		ret = idle_ret;

	for (i = 0; i < njobs; i++)
		kmem_cache_free(ge->jobs, jobs[i]);

	return ret;
}

/*
 * Compile a batch of ops and run it. Shared by the classic QUEUE (v1) and the
 * registered QUEUE2 (v2) paths; the only difference between them is how the
 * caller obtains the src/dst device addresses (per-call dma-buf map vs.
 * persistent handle), so all the op validation/optimisation/compilation lives
 * here. Every op is compiled up front, so an invalid op is rejected before
 * anything is pushed to the engine.
 *
 * src_addr == 0 means "no source" (fill/line/draw ops); src_cfg is then unused.
 * The caller guarantees num_ops is in [1, MSTAR_GE_MAX_JOBS] and that the
 * backing buffers stay mapped for the duration (this call blocks until idle).
 */
static int mstar_ge_submit_ops(struct mstar_ge *ge,
			       const struct mstar_ge_opdata *ops, int num_ops,
			       dma_addr_t src_addr,
			       const struct mstar_ge_buf_cfg *src_cfg,
			       dma_addr_t dst_addr,
			       const struct mstar_ge_buf_cfg *dst_cfg)
{
	struct mstar_ge_prog *progs;
	int i, ret = 0;

	progs = kvcalloc(num_ops, sizeof(*progs), GFP_KERNEL);
	if (!progs)
		return -ENOMEM;

	for (i = 0; i < num_ops; i++) {
		struct mstar_ge_opdata op = ops[i];

		if (mstar_ge_validate_op(ge, &op, i)) {
			ret = -EINVAL;
			goto free_progs;
		}

		/* If possible optimize the op (may rewrite the local copy) */
		if (mstar_ge_optimize_op(ge, &op) &&
		    mstar_ge_validate_op(ge, &op, i)) {
			ret = -EINVAL;
			goto free_progs;
		}

		ret = mstar_ge_compile_op(ge, &op, src_addr, src_cfg,
					  dst_addr, dst_cfg, &progs[i]);
		if (ret)
			goto free_progs;
	}

	ret = mstar_ge_run_progs(ge, progs, num_ops);

free_progs:
	kvfree(progs);
	return ret;
}

static long mstar_ge_ioctl_queue(struct mstar_ge *ge, unsigned long arg)
{
	struct mstar_ge_job_request req;
	struct mstar_ge_opdata *ops;
	struct mstar_ge_buf bufs[2];
	enum dma_data_direction dma_dirs[2];
	struct dma_buf *dma_bufs[2];
	struct dma_buf_attachment *dma_attachs[2];
	struct sg_table *dma_mappings[2];
	dma_addr_t dma_addrs[2];
	unsigned long tag = 0xAA55;
	size_t opssz, bufssz;
	int ret;
	int i;

	ret = copy_from_user(&req, (void*) arg, sizeof(req));
	if (ret) {
		dev_err(ge->dev, "Failed to get job request: %d, %lx\n", ret, arg);
		return -EFAULT;
	}

	/* Need at least one op */
	if (req.num_ops < 1) {
		dev_err(ge->dev, "Invalid amount of ops in request: %d\n",
				req.num_ops);
		return -EINVAL;
	}

	/* There is a limit to how many jobs can be queued at once */
	if (req.num_ops > MSTAR_GE_MAX_JOBS) {
		dev_err(ge->dev, "Tried to queue too many jobs\n");
		return -EINVAL;
	}

	/* For now we either need 1 or 2 buffers */
	if (req.num_bufs <= 0 || req.num_bufs > 2) {
		dev_err(ge->dev, "Invalid amount of buffers in request: %d\n",
				req.num_bufs);
		return -EINVAL;
	}

	opssz = sizeof(struct mstar_ge_opdata) * req.num_ops;
	ops = kzalloc(opssz, GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	ret = copy_from_user(ops, req.ops, opssz);
	if (ret) {
		dev_err(ge->dev, "Failed to copy ops from job request: %d\n", ret);
		return -EFAULT;
	}

	bufssz = sizeof(struct mstar_ge_buf) * req.num_bufs;
	ret = copy_from_user(bufs, req.bufs, bufssz);
	if (ret) {
		dev_err(ge->dev, "Failed to copy bufs from job request: %d\n", ret);
		goto free_ops;
	}

	/* Check all the buffers are sane */
	for (i = 0; i < req.num_bufs; i++) {
		struct mstar_ge_buf *buf = &bufs[i];
		if (mstar_ge_validate_buffer(ge, buf, i)) {
			ret = -EFAULT;
			goto free_ops;
		}
	}

	/* Figure out the DMA directions */
	if (req.num_bufs == 1)
		dma_dirs[0] = DMA_FROM_DEVICE;
	else {
		dma_dirs[0] = DMA_TO_DEVICE;
		dma_dirs[1] = DMA_FROM_DEVICE;
	}

	/* Map the buffers */
	for (i = 0; i < req.num_bufs; i++) {
		struct mstar_ge_buf *buf = &bufs[i];

		dma_bufs[i] = dma_buf_get(buf->fd);
		if (IS_ERR(dma_bufs[i])) {
			dev_err(ge->dev, "failed to get dma_buf for buffer\n");
			return PTR_ERR(dma_bufs[i]);
		}

		/*
		 * The whole surface (pitch * height) must fit within the actual
		 * imported buffer, otherwise the GE would DMA past its end and
		 * corrupt unrelated memory. pitch/height were sanity-checked in
		 * mstar_ge_validate_buffer(); this bounds them against the fd.
		 */
		if ((u64)buf->cfg.pitch * buf->cfg.height > dma_bufs[i]->size) {
			dev_err(ge->dev,
				"buffer %d geometry (%ux%u pitch %u) exceeds dma-buf size %zu\n",
				i, buf->cfg.width, buf->cfg.height, buf->cfg.pitch,
				dma_bufs[i]->size);
			dma_buf_put(dma_bufs[i]);
			while (i-- > 0) {
				dma_buf_unmap_attachment(dma_attachs[i], dma_mappings[i],
							 dma_dirs[i]);
				dma_buf_detach(dma_bufs[i], dma_attachs[i]);
				dma_buf_put(dma_bufs[i]);
			}
			ret = -EINVAL;
			goto free_ops;
		}

		dma_attachs[i] = dma_buf_attach(dma_bufs[i], ge->dev);
		if (IS_ERR(dma_attachs[i])) {
			dev_err(ge->dev, "failed to attach dma buf\n");
			return PTR_ERR(dma_attachs[i]);
		}

		dma_mappings[i] = dma_buf_map_attachment(dma_attachs[i], dma_dirs[i]);
		if (IS_ERR(dma_mappings[i])) {
			dev_err(ge->dev, "failed to map dma buf\n");
			return PTR_ERR(dma_mappings[i]);
		}

		dma_addrs[i] = sg_dma_address(dma_mappings[i]->sgl);
		dev_dbg(ge->dev, "buffer is mapped to 0x%x\n", dma_addrs[i]);
	}

	if (req.num_bufs == 1)
		ret = mstar_ge_submit_ops(ge, ops, req.num_ops,
					  0, NULL,
					  dma_addrs[0], &bufs[0].cfg);
	else
		ret = mstar_ge_submit_ops(ge, ops, req.num_ops,
					  dma_addrs[0], &bufs[0].cfg,
					  dma_addrs[1], &bufs[1].cfg);

	/* unmap everything, this needs to move once things are actually async */
	for (i = 0; i < req.num_bufs; i++) {
		dma_buf_unmap_attachment(dma_attachs[i], dma_mappings[i], dma_dirs[i]);
		dma_buf_detach(dma_bufs[i], dma_attachs[i]);
		dma_buf_put(dma_bufs[i]);
	}

	if (!ret && copy_to_user(req.tag, &tag, sizeof(tag))) {
		dev_err(ge->dev, "Failed to copy request tag to user\n");
		ret = -EFAULT;
	}

free_ops:
	kfree(ops);
	return ret;
}

/* Tear down a registered buffer's persistent mapping. Not locked. */
static void mstar_ge_reg_buf_free(struct mstar_ge_reg_buf *rb)
{
	dma_buf_unmap_attachment_unlocked(rb->attach, rb->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(rb->dma_buf, rb->attach);
	dma_buf_put(rb->dma_buf);
	kfree(rb);
}

/* Drop one reference to a handle; free the mapping when it hits zero.
 * Caller must hold gf->lock.
 */
static int mstar_ge_unregister_locked(struct mstar_ge_file *gf, u32 handle)
{
	struct mstar_ge_reg_buf *rb = idr_find(&gf->buf_idr, handle);

	if (!rb)
		return -EINVAL;

	if (--rb->refcount > 0)
		return 0;

	idr_remove(&gf->buf_idr, handle);
	mstar_ge_reg_buf_free(rb);

	return 0;
}

static long mstar_ge_ioctl_register(struct mstar_ge_file *gf, unsigned long arg)
{
	struct mstar_ge *ge = gf->ge;
	struct mstar_ge_buf_reg reg;
	struct mstar_ge_reg_buf *rb;
	struct dma_buf *dbuf;
	int id, ret;

	if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
		return -EFAULT;

	if (reg.fd < 0)
		return -EINVAL;

	dbuf = dma_buf_get(reg.fd);
	if (IS_ERR(dbuf))
		return PTR_ERR(dbuf);

	mutex_lock(&gf->lock);

	/*
	 * Dedup by dma-buf identity: registering the same underlying buffer
	 * again (even via a different fd number) returns the existing handle
	 * and just bumps the reference count. This keeps a userspace fd->handle
	 * cache correct across PRIME fd-number reuse.
	 */
	idr_for_each_entry(&gf->buf_idr, rb, id) {
		if (rb->dma_buf == dbuf) {
			rb->refcount++;
			reg.handle = id;
			dma_buf_put(dbuf);	/* drop our extra ref */
			mutex_unlock(&gf->lock);
			goto out_copy;
		}
	}

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb) {
		ret = -ENOMEM;
		goto err_put;
	}

	rb->attach = dma_buf_attach(dbuf, ge->dev);
	if (IS_ERR(rb->attach)) {
		ret = PTR_ERR(rb->attach);
		dev_err(ge->dev, "failed to attach dma buf: %d\n", ret);
		goto err_free;
	}

	rb->sgt = dma_buf_map_attachment_unlocked(rb->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(rb->sgt)) {
		ret = PTR_ERR(rb->sgt);
		dev_err(ge->dev, "failed to map dma buf: %d\n", ret);
		goto err_detach;
	}

	rb->dma_buf = dbuf;
	rb->dma_addr = sg_dma_address(rb->sgt->sgl);
	rb->size = dbuf->size;
	rb->refcount = 1;

	/* handles start at 1 so 0 is always an invalid handle */
	id = idr_alloc(&gf->buf_idr, rb, 1, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_unmap;
	}

	reg.handle = id;
	mutex_unlock(&gf->lock);

	dev_dbg(ge->dev, "registered fd %d as handle %u (addr 0x%pad size %zu)\n",
		reg.fd, id, &rb->dma_addr, rb->size);

out_copy:
	if (copy_to_user((void __user *)arg, &reg, sizeof(reg))) {
		mutex_lock(&gf->lock);
		mstar_ge_unregister_locked(gf, reg.handle);
		mutex_unlock(&gf->lock);
		return -EFAULT;
	}

	return 0;

err_unmap:
	dma_buf_unmap_attachment_unlocked(rb->attach, rb->sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dbuf, rb->attach);
err_free:
	kfree(rb);
err_put:
	mutex_unlock(&gf->lock);
	dma_buf_put(dbuf);

	return ret;
}

static long mstar_ge_ioctl_unregister(struct mstar_ge_file *gf, unsigned long arg)
{
	u32 handle;
	int ret;

	if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
		return -EFAULT;

	mutex_lock(&gf->lock);
	ret = mstar_ge_unregister_locked(gf, handle);
	mutex_unlock(&gf->lock);

	return ret;
}

static long mstar_ge_ioctl_queue2(struct mstar_ge_file *gf, unsigned long arg)
{
	struct mstar_ge *ge = gf->ge;
	struct mstar_ge_job_request2 req;
	struct mstar_ge_opdata *ops;
	struct mstar_ge_buf2 bufs[2];
	struct mstar_ge_buf_cfg cfgs[2];
	dma_addr_t addrs[2];
	unsigned long tag = 0xAA55;
	size_t opssz;
	int i, ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.flags & ~MSTAR_GE_QUEUE2_FLAGS_ALL)
		return -EINVAL;
	if (req.num_ops < 1 || req.num_ops > MSTAR_GE_MAX_JOBS)
		return -EINVAL;
	if (req.num_bufs < 1 || req.num_bufs > 2)
		return -EINVAL;

	opssz = sizeof(*ops) * req.num_ops;
	ops = kzalloc(opssz, GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	if (copy_from_user(ops, req.ops, opssz)) {
		ret = -EFAULT;
		goto free_ops;
	}

	if (copy_from_user(bufs, req.bufs,
			   sizeof(struct mstar_ge_buf2) * req.num_bufs)) {
		ret = -EFAULT;
		goto free_ops;
	}

	for (i = 0; i < req.num_bufs; i++) {
		if (mstar_ge_validate_buf_cfg(ge, &bufs[i].cfg, i)) {
			ret = -EINVAL;
			goto free_ops;
		}
	}

	/*
	 * Resolve the handles to device addresses under the file lock, then
	 * drop the lock before running - the classic path likewise holds no
	 * lock across the blocking wait. The caller must not unregister a
	 * buffer that is in flight (same contract as any GPU-style API).
	 */
	mutex_lock(&gf->lock);
	for (i = 0; i < req.num_bufs; i++) {
		struct mstar_ge_reg_buf *rb = idr_find(&gf->buf_idr,
						       bufs[i].handle);

		if (!rb) {
			dev_err(ge->dev, "unknown buffer handle %u\n",
				bufs[i].handle);
			mutex_unlock(&gf->lock);
			ret = -EINVAL;
			goto free_ops;
		}

		/* the requested surface must fit inside the mapped buffer */
		if ((u64)bufs[i].cfg.pitch * bufs[i].cfg.height > rb->size) {
			dev_err(ge->dev,
				"buffer %d geometry (%ux%u pitch %u) exceeds mapped size %zu\n",
				i, bufs[i].cfg.width, bufs[i].cfg.height,
				bufs[i].cfg.pitch, rb->size);
			mutex_unlock(&gf->lock);
			ret = -EINVAL;
			goto free_ops;
		}

		addrs[i] = rb->dma_addr;
		cfgs[i] = bufs[i].cfg;
	}
	mutex_unlock(&gf->lock);

	if (req.num_bufs == 1)
		ret = mstar_ge_submit_ops(ge, ops, req.num_ops,
					  0, NULL, addrs[0], &cfgs[0]);
	else
		ret = mstar_ge_submit_ops(ge, ops, req.num_ops,
					  addrs[0], &cfgs[0],
					  addrs[1], &cfgs[1]);

	if (!ret && req.tag && copy_to_user(req.tag, &tag, sizeof(tag))) {
		dev_err(ge->dev, "Failed to copy request tag to user\n");
		ret = -EFAULT;
	}

free_ops:
	kfree(ops);
	return ret;
}

/*
 * Drop one reference to a compiled job; on the last one release the
 * registered buffers it pinned and free it. Caller must hold gf->lock.
 */
static void mstar_ge_cjob_put_locked(struct mstar_ge_file *gf,
				     struct mstar_ge_cjob *cjob)
{
	if (--cjob->refcount > 0)
		return;

	if (cjob->src_handle)
		mstar_ge_unregister_locked(gf, cjob->src_handle);
	if (cjob->dst_handle)
		mstar_ge_unregister_locked(gf, cjob->dst_handle);

	kvfree(cjob);
}

/*
 * MSTAR_GE_IOCTL_COMPILE_JOB: run all of the per-op derivation once, store
 * the resulting register programs keyed by a token and return the token.
 * The job references buffers registered with MSTAR_GE_IOCTL_REGISTER_BUFFER,
 * exactly like QUEUE2, and takes a reference on them so their persistent
 * mappings (whose device addresses are baked into the programs) stay alive
 * until the job is freed.
 */
static long mstar_ge_ioctl_compile(struct mstar_ge_file *gf, unsigned long arg)
{
	struct mstar_ge *ge = gf->ge;
	struct mstar_ge_compile_request req;
	struct mstar_ge_opdata *ops;
	struct mstar_ge_buf2 bufs[2];
	struct mstar_ge_reg_buf *rbs[2];
	struct mstar_ge_cjob *cjob;
	dma_addr_t src_addr = 0, dst_addr;
	const struct mstar_ge_buf_cfg *src_cfg = NULL, *dst_cfg;
	size_t opssz;
	int i, ret, token;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.flags)
		return -EINVAL;
	if (req.num_ops < 1 || req.num_ops > MSTAR_GE_MAX_JOBS)
		return -EINVAL;
	if (req.num_bufs < 1 || req.num_bufs > 2)
		return -EINVAL;

	opssz = sizeof(*ops) * req.num_ops;
	ops = kzalloc(opssz, GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	if (copy_from_user(ops, req.ops, opssz)) {
		ret = -EFAULT;
		goto free_ops;
	}

	if (copy_from_user(bufs, req.bufs,
			   sizeof(struct mstar_ge_buf2) * req.num_bufs)) {
		ret = -EFAULT;
		goto free_ops;
	}

	for (i = 0; i < req.num_bufs; i++) {
		if (mstar_ge_validate_buf_cfg(ge, &bufs[i].cfg, i)) {
			ret = -EINVAL;
			goto free_ops;
		}
	}

	cjob = kvzalloc(struct_size(cjob, progs, req.num_ops), GFP_KERNEL);
	if (!cjob) {
		ret = -ENOMEM;
		goto free_ops;
	}
	cjob->num_progs = req.num_ops;
	/* the reference owned by the token table */
	cjob->refcount = 1;

	mutex_lock(&gf->lock);

	if (gf->num_cjobs >= MSTAR_GE_MAX_COMPILED_JOBS) {
		ret = -EMFILE;
		goto err_unlock;
	}

	for (i = 0; i < req.num_bufs; i++) {
		rbs[i] = idr_find(&gf->buf_idr, bufs[i].handle);
		if (!rbs[i]) {
			dev_err(ge->dev, "unknown buffer handle %u\n",
				bufs[i].handle);
			ret = -EINVAL;
			goto err_unlock;
		}

		/* the requested surface must fit inside the mapped buffer */
		if ((u64)bufs[i].cfg.pitch * bufs[i].cfg.height > rbs[i]->size) {
			dev_err(ge->dev,
				"buffer %d geometry (%ux%u pitch %u) exceeds mapped size %zu\n",
				i, bufs[i].cfg.width, bufs[i].cfg.height,
				bufs[i].cfg.pitch, rbs[i]->size);
			ret = -EINVAL;
			goto err_unlock;
		}
	}

	if (req.num_bufs == 1) {
		dst_addr = rbs[0]->dma_addr;
		dst_cfg = &bufs[0].cfg;
	} else {
		src_addr = rbs[0]->dma_addr;
		src_cfg = &bufs[0].cfg;
		dst_addr = rbs[1]->dma_addr;
		dst_cfg = &bufs[1].cfg;
	}

	for (i = 0; i < req.num_ops; i++) {
		struct mstar_ge_opdata op = ops[i];

		if (mstar_ge_validate_op(ge, &op, i) ||
		    (mstar_ge_optimize_op(ge, &op) &&
		     mstar_ge_validate_op(ge, &op, i))) {
			ret = -EINVAL;
			goto err_unlock;
		}

		ret = mstar_ge_compile_op(ge, &op, src_addr, src_cfg,
					  dst_addr, dst_cfg, &cjob->progs[i]);
		if (ret)
			goto err_unlock;
	}

	/* tokens start at 1 so 0 is always an invalid token */
	token = idr_alloc(&gf->cjob_idr, cjob, 1, 0, GFP_KERNEL);
	if (token < 0) {
		ret = token;
		goto err_unlock;
	}

	/* pin the buffers whose addresses the programs bake in */
	if (req.num_bufs == 1) {
		cjob->dst_handle = bufs[0].handle;
		rbs[0]->refcount++;
	} else {
		cjob->src_handle = bufs[0].handle;
		cjob->dst_handle = bufs[1].handle;
		rbs[0]->refcount++;
		rbs[1]->refcount++;
	}

	gf->num_cjobs++;
	mutex_unlock(&gf->lock);

	dev_dbg(ge->dev, "compiled job token %u (%u op(s))\n",
		token, cjob->num_progs);

	req.token = token;
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		mutex_lock(&gf->lock);
		idr_remove(&gf->cjob_idr, token);
		gf->num_cjobs--;
		mstar_ge_cjob_put_locked(gf, cjob);
		mutex_unlock(&gf->lock);
		ret = -EFAULT;
		goto free_ops;
	}

	kfree(ops);
	return 0;

err_unlock:
	mutex_unlock(&gf->lock);
	kvfree(cjob);
free_ops:
	kfree(ops);
	return ret;
}

/*
 * MSTAR_GE_IOCTL_FIRE_JOB: replay a pre-compiled job's register programs to
 * the engine. No validation, no derivation and no dma-buf work happens here;
 * like QUEUE/QUEUE2 this blocks until the batch has retired.
 */
static long mstar_ge_ioctl_fire(struct mstar_ge_file *gf, unsigned long arg)
{
	struct mstar_ge *ge = gf->ge;
	struct mstar_ge_fire_request req;
	struct mstar_ge_cjob *cjob;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.flags)
		return -EINVAL;

	mutex_lock(&gf->lock);
	cjob = idr_find(&gf->cjob_idr, req.token);
	if (!cjob) {
		mutex_unlock(&gf->lock);
		return -EINVAL;
	}
	/*
	 * Keep the job (and the buffer mappings it pins) alive while it is
	 * on the engine, even if another thread frees the token meanwhile.
	 */
	cjob->refcount++;
	mutex_unlock(&gf->lock);

	ret = mstar_ge_run_progs(ge, cjob->progs, cjob->num_progs);

	mutex_lock(&gf->lock);
	mstar_ge_cjob_put_locked(gf, cjob);
	mutex_unlock(&gf->lock);

	return ret;
}

/* MSTAR_GE_IOCTL_FREE_JOB: drop a compiled job and the buffers it pins */
static long mstar_ge_ioctl_free_job(struct mstar_ge_file *gf, unsigned long arg)
{
	struct mstar_ge_cjob *cjob;
	u32 token;

	if (copy_from_user(&token, (void __user *)arg, sizeof(token)))
		return -EFAULT;

	mutex_lock(&gf->lock);
	cjob = idr_find(&gf->cjob_idr, token);
	if (!cjob) {
		mutex_unlock(&gf->lock);
		return -EINVAL;
	}

	idr_remove(&gf->cjob_idr, token);
	gf->num_cjobs--;
	mstar_ge_cjob_put_locked(gf, cjob);
	mutex_unlock(&gf->lock);

	return 0;
}

static int mstar_ge_open(struct inode *inode, struct file *f)
{
	struct mstar_ge *ge = container_of(f->private_data,
					   struct mstar_ge, ge_dev);
	struct mstar_ge_file *gf;

	gf = kzalloc(sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;

	gf->ge = ge;
	idr_init(&gf->buf_idr);
	idr_init(&gf->cjob_idr);
	mutex_init(&gf->lock);

	f->private_data = gf;

	return 0;
}

static int mstar_ge_release(struct inode *inode, struct file *f)
{
	struct mstar_ge_file *gf = f->private_data;
	struct mstar_ge_reg_buf *rb;
	struct mstar_ge_cjob *cjob;
	int id;

	mutex_lock(&gf->lock);

	/*
	 * Free the compiled jobs first - they hold references on registered
	 * buffers. No fire can be in flight here (release runs once the last
	 * reference to the file is gone), so each job's refcount is exactly
	 * the token table's and the put frees it.
	 */
	idr_for_each_entry(&gf->cjob_idr, cjob, id)
		mstar_ge_cjob_put_locked(gf, cjob);
	idr_destroy(&gf->cjob_idr);

	/* Drop any buffers the caller left registered. */
	idr_for_each_entry(&gf->buf_idr, rb, id)
		mstar_ge_reg_buf_free(rb);
	idr_destroy(&gf->buf_idr);
	mutex_unlock(&gf->lock);

	mutex_destroy(&gf->lock);
	kfree(gf);

	return 0;
}

static long mstar_ge_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct mstar_ge_file *gf = f->private_data;
	struct mstar_ge *ge = gf->ge;
	int ret = 0;

	switch(cmd){
	case MSTAR_GE_IOCTL_INFO: {
		struct mstar_ge_info info;
		/*
		 * Feature discovery: bit 0 has been set since ABI v1
		 * (caps == 1), so old userspace sees exactly the value it
		 * always did in the bits it knows about.
		 */
		info.caps = MSTAR_GE_CAP_QUEUE |
			    MSTAR_GE_CAP_BUFFER_REGISTRATION |
			    MSTAR_GE_CAP_COMPILED_JOBS;
		ret = copy_to_user((void *) arg, &info, sizeof(info));
	}
		break;
	case MSTAR_GE_IOCTL_QUEUE:
		return mstar_ge_ioctl_queue(ge, arg);
	case MSTAR_GE_IOCTL_REGISTER_BUFFER:
		return mstar_ge_ioctl_register(gf, arg);
	case MSTAR_GE_IOCTL_UNREGISTER_BUFFER:
		return mstar_ge_ioctl_unregister(gf, arg);
	case MSTAR_GE_IOCTL_QUEUE2:
		return mstar_ge_ioctl_queue2(gf, arg);
	case MSTAR_GE_IOCTL_COMPILE_JOB:
		return mstar_ge_ioctl_compile(gf, arg);
	case MSTAR_GE_IOCTL_FIRE_JOB:
		return mstar_ge_ioctl_fire(gf, arg);
	case MSTAR_GE_IOCTL_FREE_JOB:
		return mstar_ge_ioctl_free_job(gf, arg);
	case MSTAR_GE_IOCTL_QUERY: {
		unsigned long tag;

		if (copy_from_user(&tag, (void __user *) arg, sizeof(tag)))
			return -EFAULT;

	}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations ge_fops = {
	.owner		= THIS_MODULE,
	.open		= mstar_ge_open,
	.release	= mstar_ge_release,
	.unlocked_ioctl	= mstar_ge_ioctl,
};

#if defined(CONFIG_PM_DEVFREQ)
static int mstar_ge_target(struct device *dev,
		unsigned long *freq, u32 flags)
{
	struct mstar_ge *ge = dev_get_drvdata(dev);

	dev_info(ge->dev, "%s:%d\n", __func__, __LINE__);

	return 0;
}

static int mstar_ge_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct mstar_ge *ge = dev_get_drvdata(dev);

	*freq = clk_get_rate(ge->clk);

	return 0;
}
#endif

static int mstar_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mstar_ge *ge;
	void __iomem *base;
	int irq, ret;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	ge->jobs = kmem_cache_create("gejobs", sizeof(struct mstar_ge_job),
			__alignof__(struct mstar_ge_job), 0, mstar_ge_job_init);
	if (!ge->jobs)
		return -ENOMEM;

	spin_lock_init(&ge->lock);
	INIT_LIST_HEAD(&ge->queue);
	init_waitqueue_head(&ge->dma_wait);

	ge->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_ge_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ge->regmap = regmap;

	ge->en = devm_regmap_field_alloc(dev, regmap, en_field);
	ge->abl = devm_regmap_field_alloc(dev, regmap, abl_field);
	ge->dfb = devm_regmap_field_alloc(dev, regmap, dfb_field);
	ge->calc_srcwh = devm_regmap_field_alloc(dev, regmap, calc_srcwh_field);
	ge->clk_en = devm_regmap_field_alloc(dev, regmap, clk_en_field);
	ge->busy = devm_regmap_field_alloc(dev, regmap, gebusy_field);
	ge->cmq_free = devm_regmap_field_alloc(dev, regmap, cmq_free_field);
	ge->cmq2_free = devm_regmap_field_alloc(dev, regmap, cmq2_free_field);
	ge->irq_mask = devm_regmap_field_alloc(dev, regmap, irq_mask_field);
	ge->irq_force = devm_regmap_field_alloc(dev, regmap, irq_force_field);
	ge->irq_clr = devm_regmap_field_alloc(dev, regmap, irq_clr_field);
	ge->irq_status = devm_regmap_field_alloc(dev, regmap, irq_status_field);

	/* src buffer */
	ge->srcl = devm_regmap_field_alloc(dev, regmap, srcl_field);
	ge->srch = devm_regmap_field_alloc(dev, regmap, srch_field);
	ge->srcpitch = devm_regmap_field_alloc(dev, regmap, srcpitch_field);
	ge->srcclrfmt = devm_regmap_field_alloc(dev, regmap, src_colorfmt_field);

	/* dst buffer */
	ge->dstl = devm_regmap_field_alloc(dev, regmap, dstl_field);
	ge->dsth = devm_regmap_field_alloc(dev, regmap, dsth_field);
	ge->dstpitch = devm_regmap_field_alloc(dev, regmap, dstpitch_field);
	ge->dstclrfmt = devm_regmap_field_alloc(dev, regmap, dst_colorfmt_field);

	/* stretch blit */
	ge->stbb_en = devm_regmap_field_alloc(dev, regmap, stbb_en_field);
	ge->stbb_ini_dx = devm_regmap_field_alloc(dev, regmap, stbb_ini_dx_field);
	ge->stbb_ini_dy = devm_regmap_field_alloc(dev, regmap, stbb_ini_dy_field);
	ge->stbb_dx = devm_regmap_field_alloc(dev, regmap, stbb_dx_field);
	ge->stbb_dy = devm_regmap_field_alloc(dev, regmap, stbb_dy_field);
	ge->bitblt_src_width = devm_regmap_field_alloc(dev, regmap, bitblt_srcwidth_field);
	ge->bitblt_src_height = devm_regmap_field_alloc(dev, regmap, bitblt_srcheight_field);

	ge->bld_alphachan = devm_regmap_field_alloc(dev, regmap, bld_alphachan_field);

	/* source colour key */
	ge->sck_en = devm_regmap_field_alloc(dev, regmap, sck_en_field);
	ge->sck_op_mode = devm_regmap_field_alloc(dev, regmap, sck_op_mode_field);
	ge->sck_hth_l = devm_regmap_field_alloc(dev, regmap, sck_hth_l_field);
	ge->sck_hth_h = devm_regmap_field_alloc(dev, regmap, sck_hth_h_field);
	ge->sck_lth_l = devm_regmap_field_alloc(dev, regmap, sck_lth_l_field);
	ge->sck_lth_h = devm_regmap_field_alloc(dev, regmap, sck_lth_h_field);

	/* gradient fill */
	ge->rect_ch = devm_regmap_field_alloc(dev, regmap, rect_ch_field);
	ge->rect_cv = devm_regmap_field_alloc(dev, regmap, rect_cv_field);

	/* stretch blit filter type */
	ge->stbb_type = devm_regmap_field_alloc(dev, regmap, stbb_type_field);

	/* clipping window */
	ge->clip_left = devm_regmap_field_alloc(dev, regmap, clip_left_field);
	ge->clip_right = devm_regmap_field_alloc(dev, regmap, clip_right_field);
	ge->clip_top = devm_regmap_field_alloc(dev, regmap, clip_top_field);
	ge->clip_bottom = devm_regmap_field_alloc(dev, regmap, clip_bottom_field);

	ge->rot = devm_regmap_field_alloc(dev, regmap, rot_field);
	ge->prim_type = devm_regmap_field_alloc(dev, regmap, prim_type_field);
	ge->pri_s_y_dir = devm_regmap_field_alloc(dev, regmap, pri_s_y_dir_field);
	ge->pri_x_dir = devm_regmap_field_alloc(dev, regmap, pri_x_dir_field);
	ge->pri_y_dir = devm_regmap_field_alloc(dev, regmap, pri_y_dir_field);

	/* Line controls */
	ge->line_delta = devm_regmap_field_alloc(dev, regmap, line_delta_field);
	ge->line_major = devm_regmap_field_alloc(dev, regmap, line_major_field);
	ge->line_last = devm_regmap_field_alloc(dev, regmap, line_last_field);
	ge->line_length = devm_regmap_field_alloc(dev, regmap, line_length_field);

	/* vertex */
	ge->x0 = devm_regmap_field_alloc(dev, regmap, x0_field);
	ge->y0 = devm_regmap_field_alloc(dev, regmap, y0_field);
	ge->x1 = devm_regmap_field_alloc(dev, regmap, x1_field);
	ge->y1 = devm_regmap_field_alloc(dev, regmap, y1_field);
	ge->x2 = devm_regmap_field_alloc(dev, regmap, x2_field);
	ge->y2 = devm_regmap_field_alloc(dev, regmap, y2_field);

	/* start color */
	ge->b_st = devm_regmap_field_alloc(dev, regmap, b_st_field);
	ge->g_st = devm_regmap_field_alloc(dev, regmap, g_st_field);
	ge->r_st = devm_regmap_field_alloc(dev, regmap, r_st_field);
	ge->a_st = devm_regmap_field_alloc(dev, regmap, a_st_field);

	ge->tagl = devm_regmap_field_alloc(dev, regmap, tagl_field);
	ge->tagh = devm_regmap_field_alloc(dev, regmap, tagh_field);

	/* p256 */
	ge->p256_b = devm_regmap_field_alloc(dev, regmap, p256_b_field);
	ge->p256_g = devm_regmap_field_alloc(dev, regmap, p256_g_field);
	ge->p256_r = devm_regmap_field_alloc(dev, regmap, p256_r_field);
	ge->p256_a = devm_regmap_field_alloc(dev, regmap, p256_a_field);
	ge->p256_index = devm_regmap_field_alloc(dev, regmap, p256_index_field);
	ge->p256_rw = devm_regmap_field_alloc(dev, regmap, p256_rw_field);

	ge->clk = devm_clk_get(dev, "ge");
	if (IS_ERR(ge->clk))
		return PTR_ERR(ge->clk);

	clk_prepare_enable(ge->clk);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	ret = devm_request_irq(dev, irq, mstar_ge_irq, IRQF_SHARED, dev_name(dev), ge);
	if (ret)
		return ret;

	regmap_field_write(ge->irq_mask, 0);
	regmap_field_write(ge->clk_en, 1);

	dev_set_drvdata(dev, ge);

	/*
	 * Keep the engine resumed across a burst of jobs instead of suspending
	 * the instant the queue drains: a DirectFB frame submits many ops in
	 * quick succession and per-job resume/suspend churn is pure overhead.
	 */
	pm_runtime_set_autosuspend_delay(dev, MSTAR_GE_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);

	/* pm runtime must be enabled before self testing */
	pm_runtime_enable(dev);

	if (IS_ENABLED(CONFIG_DRM_MSTAR_GE_SELFTEST)) {
		int testret = mstar_ge_test(ge);

		if (testret)
			dev_err(dev, "Self test failed: %d\n", testret);
	}

	/* Finally register the misc device so userspace can use this */
	ge->ge_dev.minor = MISC_DYNAMIC_MINOR;
	ge->ge_dev.name	= DRIVER_NAME;
	ge->ge_dev.fops	= &ge_fops;

#if defined(CONFIG_PM_DEVFREQ)
	ret = dev_pm_opp_of_add_table(dev);
	if (ret < 0) {
		dev_err(dev, "failed to get OPP table\n");
		return ret;
	}

	ge->profile.target = mstar_ge_target;
	ge->profile.get_cur_freq = mstar_ge_get_cur_freq;
	ge->profile.initial_freq = clk_get_rate(ge->clk);

	ge->devfreq = devm_devfreq_add_device(dev,
					      &ge->profile,
					      DEVFREQ_GOV_USERSPACE,
					      NULL);
	if (IS_ERR(ge->devfreq)) {
		ret = PTR_ERR(ge->devfreq);
		dev_err(dev, "failed to add devfreq device: %d\n", ret);
		return ret;
	}
#endif

	return misc_register(&ge->ge_dev);
}

static void mstar_ge_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_ge *ge = dev_get_drvdata(dev);

	pm_runtime_force_suspend(dev);

	misc_deregister(&ge->ge_dev);

	kmem_cache_destroy(ge->jobs);
}

static const struct of_device_id mstar_ge_ids[] = {
	{
		.compatible = "sstar,ssd20xd-ge",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_ge_ids);

static int __maybe_unused mstar_ge_runtime_suspend(struct device *dev)
{
	//struct msc313e_i2c *i2c = dev_get_drvdata(dev);

	return 0;
}

static int __maybe_unused mstar_ge_runtime_resume(struct device *dev)
{
	//struct msc313e_i2c *i2c = dev_get_drvdata(dev);
	//long clk_rate;

	return 0;
};

static const struct dev_pm_ops mstar_ge_pm = {
	SET_RUNTIME_PM_OPS(mstar_ge_runtime_suspend, mstar_ge_runtime_resume, NULL)
};

static struct platform_driver mstar_ge_driver = {
	.probe = mstar_ge_probe,
	.remove = mstar_ge_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_ge_ids,
		.pm = &mstar_ge_pm,
	},
};
module_platform_driver(mstar_ge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
