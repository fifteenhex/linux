#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of.h>

#define DRIVER_NAME "mstar-mop"

#define ADDR_SHIFT	4

static const uint32_t mop_formats[] = {
	DRM_FORMAT_NV12,
};

static const uint64_t mop_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

struct mstar_mop;

struct mstar_mop_window {
	struct mstar_mop *mop;
	struct regmap_field *en;
	struct regmap_field *yaddrl, *yaddrh;
	struct regmap_field *caddrl, *caddrh;
	struct regmap_field *hst;
	struct regmap_field *hend;
	struct regmap_field *vst;
	struct regmap_field *vend;
	struct regmap_field *pitch;
	struct regmap_field *src_width;
	struct regmap_field *src_height;
	struct regmap_field *scale_h;
	struct regmap_field *scale_v;
	struct regmap_field *ctrl;
	/* what this window was last programmed with, for the line buffer split */
	bool enabled;
	unsigned int hstart;
	struct drm_plane drm_plane;
};

#define plane_to_mop_window(plane) container_of(plane, struct mstar_mop_window, drm_plane)

struct mstar_mop_data {
	unsigned int num_windows;
	unsigned int windows_start;
	unsigned int window_len;
};

struct mstar_mop {
	struct device *dev;
	const struct mstar_mop_data *data;
	struct regmap *regmap;
	/* the shared double buffer trigger, which may live in another instance */
	struct regmap *latch_regmap;
	struct regmap_field *swrst;
	struct regmap_field *gw_hsize;
	struct regmap_field *gw_vsize;
	struct regmap_field *commit_all;
	struct mstar_mop_window windows[];
};

static const struct reg_field swrst_field = REG_FIELD(0x0, 0, 0);
static const struct reg_field gw_hsize_field = REG_FIELD(0x1c, 0, 12);
static const struct reg_field gw_vsize_field = REG_FIELD(0x20, 0, 12);
static const struct reg_field commit_all_field = REG_FIELD(0x1fc, 8, 8);

/*
 * Bank 0, the plane's global config. The MOP only fetches once the two clock
 * gates in CFG are open, so without this block the windows can be programmed
 * and enabled and still nothing is read from DRAM. Values are the vendor's
 * HalDispMopgInit.
 */
#define MOP_REG_CFG		0x004
#define  MOP_CFG_CLK_MIU	BIT(8)
#define  MOP_CFG_CLK_GOP	BIT(9)
#define  MOP_CFG_AUTOSTRETCH	BIT(10)
#define  MOP_CFG_AUTOBLANK	BIT(11)
#define MOP_REG_PIPEDLY		0x00c
#define MOP_REG_YDMA_THD	0x010
#define MOP_REG_CDMA_THD	0x014
#define MOP_REG_Y_PRIO		0x018
#define MOP_REG_C_PRIO		0x02c
#define MOP_REG_GW_HEXT		0x054
#define MOP_REG_4TAP		0x100

/*
 * Bank 1+, per window. Offsets from the window base; see mstar_mop_data.
 *
 * CTRL holds three fields that the vendor sets one at a time through a software
 * shadow: bits 7:0 are the line buffer start (HalDispMopgSetLineBufStr), 12:9
 * the addr16 offset (HalDispMopgSetAddr16Offset) and 15:9 the rblk horizontal
 * start (HalDispMopgSetRblkHstr). Only the line buffer start is used here, and
 * it is written whole rather than read-modify-written: mhal shadows this
 * register in software instead of reading it, and the window registers do read
 * back as zero once a window has been taken down and put up again.
 */
#define MOP_WIN_CTRL		0x04
#define  MOP_WIN_LB_UNIT	8	/* line buffer start is in 8-pixel units */
#define  MOP_WIN_LB_GAP		2	/* and gets a 2-unit gap per window */

/*
 * The plane's DMA addresses are relative to the MIU0 base and counted in
 * 16-byte units, not CPU physical addresses.
 */
#define MOP_MIU0_BASE		0x20000000
#define MOP_SCALE_1X		0x1000
/*
 * The scale registers are thirteen bits wide - write 0x3fff and 0x1fff reads
 * back - so the largest source:destination ratio the hardware can hold is
 * 8191:4096, a whisker under 2x. A 2x shrink needs 8192, which does not fit:
 * regmap masks it to zero and the window scans out garbage, which is a very
 * confusing way to find this out.
 */
#define MOP_SCALE_MAX		0x1fff
#define MOP_SCALE_MAX_16_16	(((MOP_SCALE_MAX) << 16) / (MOP_SCALE_1X))

/*
 * There is one double buffer trigger for the whole MOP and it sits in the
 * graphics plane's page, at 0xfd280bfc. mhal's HalDispMopDbBfWr() writes that
 * one address after programming anything, the sub plane included, and the sub
 * plane's own page has no equivalent: the sub plane was drawing nothing at all
 * until it was pointed at the real trigger, and pulsing the same offset in its
 * own page - 0xfd2811fc - latches nothing.
 *
 * So an instance whose trigger is somewhere else names the instance that has it.
 */
static struct regmap *mstar_mop_latch_regmap(struct device *dev, struct regmap *own)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct mstar_mop *owner;

	np = of_parse_phandle(dev->of_node, "sstar,mop-latch", 0);
	if (!np)
		return own;

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	owner = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	if (!owner)
		return ERR_PTR(-EPROBE_DEFER);

	return owner->regmap;
}

static const struct regmap_config mstar_mop_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static void mstar_mop_dump_window(struct device *dev, struct mstar_mop_window *win)
{
	unsigned int en;
	unsigned int yaddrl, yaddrh;
	unsigned int caddrl, caddrh;
	unsigned int hst, hend;
	unsigned int vst, vend;
	unsigned int pitch;
	unsigned int srcw, srch;
	unsigned int scaleh, scalev;
	u32 yaddr, caddr;

	regmap_field_read(win->en, &en);

	regmap_field_read(win->yaddrl, &yaddrl);
	regmap_field_read(win->yaddrh, &yaddrh);
	yaddr = (yaddrh << 16 | yaddrl) << ADDR_SHIFT;

	regmap_field_read(win->caddrl, &caddrl);
	regmap_field_read(win->caddrh, &caddrh);
	caddr = (caddrh << 16 | caddrl) << ADDR_SHIFT;

	regmap_field_read(win->hst, &hst);
	regmap_field_read(win->hend, &hend);
	regmap_field_read(win->vst, &vst);
	regmap_field_read(win->vend, &vend);
	regmap_field_read(win->pitch, &pitch);
	regmap_field_read(win->src_width, &srcw);
	regmap_field_read(win->src_height, &srch);
	regmap_field_read(win->scale_h, &scaleh);
	regmap_field_read(win->scale_v, &scalev);

	dev_dbg(dev, "Window dump\n"
		      "enabled: %d\n"
		      "yaddr: 0x%08x\n"
		      "caddr: 0x%08x\n"
		      "horizontal start: %d, end %d\n"
		      "vertical start: %d, end %d\n"
		      "pitch: %d\n"
		      "source width: %d, height: %d\n"
		      "scale horizontal: %d, vertical: %d\n",
		      en,
		      yaddr,
		      caddr,
		      hst, hend,
		      vst, vend,
		      pitch,
		      srcw, srch,
		      scaleh, scalev);
}

/*
 * The shadowed window registers only take effect when the double-buffer write
 * bit is pulsed, so a whole window can be reprogrammed and then latched in one
 * go at the next vsync - which is what makes a per-frame update tear free.
 */
static void mstar_mop_latch(struct mstar_mop *mop)
{
	regmap_field_force_write(mop->commit_all, 1);
	regmap_field_force_write(mop->commit_all, 0);
}

/*
 * The vendor's HalDispMopgInit: a reset pulse, then the two clock gates that
 * let the plane fetch from the MIU at all, auto stretch and auto blank, the
 * pipe delay and the DMA thresholds and priorities. Nothing is fetched from
 * DRAM until CFG's gates are open, so a window can otherwise be fully
 * programmed and enabled and still show nothing.
 *
 * This is run for the sub plane too. HalDispMopsInit is instruction for
 * instruction the same sequence at the sub plane's own base, and skipping it was
 * leaving that block's clock gates shut.
 */
static void mstar_mop_hw_init(struct mstar_mop *mop)
{
	regmap_field_force_write(mop->swrst, 1);
	regmap_field_force_write(mop->swrst, 0);

	regmap_write(mop->regmap, MOP_REG_CFG,
		     MOP_CFG_CLK_MIU | MOP_CFG_CLK_GOP |
		     MOP_CFG_AUTOSTRETCH | MOP_CFG_AUTOBLANK);
	regmap_write(mop->regmap, MOP_REG_PIPEDLY, 0x000a);
	regmap_write(mop->regmap, MOP_REG_YDMA_THD, 0x00f8);
	regmap_write(mop->regmap, MOP_REG_CDMA_THD, 0x00d0);
	regmap_write(mop->regmap, MOP_REG_Y_PRIO, 0x00f0);
	regmap_write(mop->regmap, MOP_REG_C_PRIO, 0x00f0);
	regmap_write(mop->regmap, MOP_REG_GW_HEXT, 0x0080);
	regmap_write(mop->regmap, MOP_REG_4TAP, 0x0689);

	mstar_mop_latch(mop);
}

/*
 * All of the windows fetch through one shared horizontal line buffer and each
 * has to be told where its own slice of it starts. Left at slice 0, only one of
 * any set of windows that share scanlines gets drawn - which is what limited a
 * screen full of windows to four horizontal bands, one window each.
 *
 * The split is the vendor's, from _HalDispIfSetMopgLineBufOrder() and
 * _HalDispIfSetAllMopgLineBufVal(): take the enabled windows in order of
 * horizontal start, number them as you go (windows starting at the same place
 * share a number), and give each one order * gap + hstart / unit, except that
 * the first one gets 0. Every window is rewritten whenever any of them changes,
 * as the vendor does, since adding a window can renumber the rest.
 */
static void mstar_mop_line_buffers(struct mstar_mop *mop)
{
	unsigned long done = 0;
	unsigned int i, n, order = 0;
	unsigned int prev = 0;

	for (n = 0; n < mop->data->num_windows; n++) {
		struct mstar_mop_window *best = NULL;
		unsigned int besti = 0;

		for (i = 0; i < mop->data->num_windows; i++) {
			struct mstar_mop_window *window = &mop->windows[i];

			if (!window->enabled || test_bit(i, &done))
				continue;
			if (!best || window->hstart < best->hstart) {
				best = window;
				besti = i;
			}
		}
		if (!best)
			break;

		__set_bit(besti, &done);
		if (n && best->hstart != prev)
			order++;
		regmap_field_write(best->ctrl, order ?
				   order * MOP_WIN_LB_GAP +
				   best->hstart / MOP_WIN_LB_UNIT : 0);
		prev = best->hstart;
	}

	mstar_mop_latch(mop);
}

static int mop_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (!crtc_state)
		return -EINVAL;

	/*
	 * The scaler takes source:destination as a 1/4096ths ratio in a thirteen
	 * bit register, so it shrinks by up to 8191/4096 - not quite 2x - and
	 * only ever stretches to 1x. Let the helper reject anything outside that
	 * rather than silently programming a bad ratio: asking for exactly 2x
	 * (640x480 into 320x240, the obvious thing to want) is outside it.
	 */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   MOP_SCALE_MAX_16_16,
						   true, true);
}

static void mstar_mop_win_addr(struct mstar_mop_window *window,
			       struct regmap_field *lo, struct regmap_field *hi,
			       dma_addr_t addr)
{
	u32 a = ((u32)addr - MOP_MIU0_BASE) >> ADDR_SHIFT;

	regmap_field_write(lo, a & 0xffff);
	regmap_field_write(hi, (a >> 16) & 0xfff);
}

static void mstar_mop_plane_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct mstar_mop_window *window = plane_to_mop_window(plane);
	struct mstar_mop *mop = window->mop;
	struct drm_framebuffer *fb = new_state->fb;
	u32 srcw, srch, dstw, dsth;

	if (!new_state->crtc || !fb) {
		regmap_field_write(window->en, 0);
		mstar_mop_latch(mop);
		window->enabled = false;
		mstar_mop_line_buffers(mop);
		return;
	}

	srcw = drm_rect_width(&new_state->src) >> 16;
	srch = drm_rect_height(&new_state->src) >> 16;
	dstw = drm_rect_width(&new_state->dst);
	dsth = drm_rect_height(&new_state->dst);
	if (!srcw || !srch || !dstw || !dsth)
		return;

	/* source geometry, then the window it lands in on the mixer */
	regmap_field_write(window->src_width, srcw - 1);
	regmap_field_write(window->src_height, srch - 1);
	mstar_mop_latch(mop);

	regmap_field_write(window->hst, new_state->dst.x1);
	regmap_field_write(window->hend, new_state->dst.x2 - 1);
	regmap_field_write(window->vst, new_state->dst.y1);
	regmap_field_write(window->vend, new_state->dst.y2 - 1);
	mstar_mop_latch(mop);

	/*
	 * Scale is source:destination in 1/4096ths, so equal sizes give 1x. The
	 * atomic check has already refused anything the register cannot hold;
	 * clamp anyway, because the failure mode of not clamping is a window full
	 * of nothing rather than a slightly wrong picture.
	 */
	regmap_field_write(window->scale_h,
			   min((srcw * MOP_SCALE_1X) / dstw, MOP_SCALE_MAX));
	regmap_field_write(window->scale_v,
			   min((srch * MOP_SCALE_1X) / dsth, MOP_SCALE_MAX));
	mstar_mop_latch(mop);

	/* NV12: plane 0 is the luma, plane 1 the interleaved chroma */
	mstar_mop_win_addr(window, window->yaddrl, window->yaddrh,
			   drm_fb_dma_get_gem_addr(fb, new_state, 0));
	mstar_mop_win_addr(window, window->caddrl, window->caddrh,
			   drm_fb_dma_get_gem_addr(fb, new_state, 1));
	mstar_mop_latch(mop);

	regmap_field_write(window->pitch, (fb->pitches[0] >> ADDR_SHIFT) & 0x1fff);
	mstar_mop_latch(mop);

	/*
	 * Slice up the line buffer before enabling, which is the vendor's order:
	 * it does this from SetInputPortAttr, and SetInputPortEnable only turns
	 * the window on afterwards.
	 */
	window->hstart = new_state->dst.x1;
	window->enabled = true;
	mstar_mop_line_buffers(mop);

	regmap_field_write(window->en, 1);
	mstar_mop_latch(mop);

	mstar_mop_dump_window(mop->dev, window);
}

static const struct drm_plane_helper_funcs mop_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = mop_plane_atomic_check,
	.atomic_update = mstar_mop_plane_atomic_update,
};

static const struct drm_plane_funcs mop_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static int mstar_mop_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_mop *mop = dev_get_drvdata(dev);
	struct drm_device *drm_device = data;
	int i, ret;

	for(i = 0; i < mop->data->num_windows; i++) {
		struct mstar_mop_window *window = &mop->windows[i];

		ret = drm_universal_plane_init(drm_device,
					     &window->drm_plane,
					     0,
					     &mop_plane_funcs,
					     mop_formats,
					     ARRAY_SIZE(mop_formats),
					     mop_format_modifiers,
					     DRM_PLANE_TYPE_OVERLAY,
					     "window %d", i);
		if(ret)
			return ret;

		drm_plane_helper_add(&window->drm_plane, &mop_plane_helper_funcs);
	}

	return 0;
}

static void mstar_mop_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct mstar_mop *mop = dev_get_drvdata(dev);
	int i;

	for(i = 0; i < mop->data->num_windows; i++)
		drm_plane_cleanup(&mop->windows[i].drm_plane);
}

static const struct component_ops mstar_mop_component_ops = {
	.bind	= mstar_mop_bind,
	.unbind	= mstar_mop_unbind,
};

static int mstar_mop_probe(struct platform_device *pdev)
{
	const struct mstar_mop_data *match_data;
	struct device *dev = &pdev->dev;
	unsigned int hsize, vsize;
	struct mstar_mop *mop;
	struct regmap *regmap;
	void __iomem *base;
	int i, ret;

	match_data = of_device_get_match_data(dev);
	if (!match_data)
		return -EINVAL;

	mop = devm_kzalloc(dev, struct_size(mop, windows, match_data->num_windows), GFP_KERNEL);
	if (!mop)
		return -ENOMEM;

	mop->dev = dev;
	mop->data = match_data;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_mop_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* CLK_mop (the mopg has it; the mops shares it) */
	{
		struct clk *clk = devm_clk_get_optional_enabled(dev, NULL);

		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk), "Failed to get the MOP clock\n");
	}

	mop->regmap = regmap;

	mop->latch_regmap = mstar_mop_latch_regmap(dev, regmap);
	if (IS_ERR(mop->latch_regmap))
		return dev_err_probe(dev, PTR_ERR(mop->latch_regmap),
				     "Failed to find the MOP double buffer trigger\n");

	mop->swrst = devm_regmap_field_alloc(dev, regmap, swrst_field);
	mop->gw_hsize = devm_regmap_field_alloc(dev, regmap, gw_hsize_field);
	mop->gw_vsize = devm_regmap_field_alloc(dev, regmap, gw_vsize_field);
	mop->commit_all = devm_regmap_field_alloc(dev, mop->latch_regmap,
						  commit_all_field);

	mstar_mop_hw_init(mop);

	regmap_field_read(mop->gw_hsize, &hsize);
	regmap_field_read(mop->gw_vsize, &vsize);
	dev_info(dev, "MStar MOP\n"
		      "global window size; height: %d, width: %d\n",
		      hsize, vsize
		);

	for (i = 0; i < match_data->num_windows; i++){
		unsigned int offset = match_data->windows_start + (match_data->window_len * i);
		unsigned int en;

		struct reg_field en_field = REG_FIELD(offset + 0, 0, 0);
		struct reg_field yaddrl_field = REG_FIELD(offset + 0x8, 0, 15);
		struct reg_field yaddrh_field = REG_FIELD(offset + 0xc, 0, 11);
		struct reg_field caddrl_field = REG_FIELD(offset + 0x10, 0, 15);
		struct reg_field caddrh_field = REG_FIELD(offset + 0x14, 0, 11);
		struct reg_field hst_field = REG_FIELD(offset + 0x18, 0, 12);
		struct reg_field hend_field = REG_FIELD(offset + 0x1c, 0, 12);
		struct reg_field vst_field = REG_FIELD(offset + 0x20, 0, 12);
		struct reg_field vend_field = REG_FIELD(offset + 0x24, 0, 12);
		struct reg_field pitch_field = REG_FIELD(offset + 0x28, 0, 12);
		struct reg_field ctrl_field = REG_FIELD(offset + MOP_WIN_CTRL, 0, 15);
		struct reg_field srcw_field = REG_FIELD(offset + 0x2c, 0, 12);
		struct reg_field srch_field = REG_FIELD(offset + 0x30, 0, 12);
		struct reg_field scaleh_field = REG_FIELD(offset + 0x34, 0, 12);
		struct reg_field scalev_field = REG_FIELD(offset + 0x38, 0, 12);
		struct mstar_mop_window *window = &mop->windows[i];

		window->mop = mop;

		window->en = devm_regmap_field_alloc(dev, regmap, en_field);
		window->yaddrl = devm_regmap_field_alloc(dev, regmap, yaddrl_field);
		window->yaddrh = devm_regmap_field_alloc(dev, regmap, yaddrh_field);
		window->caddrl = devm_regmap_field_alloc(dev, regmap, caddrl_field);
		window->caddrh = devm_regmap_field_alloc(dev, regmap, caddrh_field);
		window->hst = devm_regmap_field_alloc(dev, regmap, hst_field);
		window->hend = devm_regmap_field_alloc(dev, regmap, hend_field);
		window->vst = devm_regmap_field_alloc(dev, regmap, vst_field);
		window->vend = devm_regmap_field_alloc(dev, regmap, vend_field);
		window->pitch = devm_regmap_field_alloc(dev, regmap, pitch_field);
		window->src_width = devm_regmap_field_alloc(dev, regmap, srcw_field);
		window->src_height = devm_regmap_field_alloc(dev, regmap, srch_field);
		window->scale_h = devm_regmap_field_alloc(dev, regmap, scaleh_field);
		window->scale_v = devm_regmap_field_alloc(dev, regmap, scalev_field);
		window->ctrl = devm_regmap_field_alloc(dev, regmap, ctrl_field);

		/* no window is on yet, so no line buffer is claimed either */
		regmap_field_write(window->ctrl, 0);

		mstar_mop_dump_window(dev, window);
	}

	dev_set_drvdata(dev, mop);

	return component_add(&pdev->dev, &mstar_mop_component_ops);
}

static void mstar_mop_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_mop_component_ops);
}

/*
 * The sleep reset takes the whole block down - every register reads back as zero
 * afterwards, including the CFG clock gates, so windows programmed after a
 * resume are written into dead hardware and nothing is fetched. Put the global
 * config back in the noirq phase, before the DRM master's resume redoes the mode
 * set and reprograms the windows. Nothing here needs the windows' own state: the
 * mode set brings that back, and no window is on until it does.
 */
static int mstar_mop_resume_noirq(struct device *dev)
{
	struct mstar_mop *mop = dev_get_drvdata(dev);
	unsigned int i;

	mstar_mop_hw_init(mop);

	for (i = 0; i < mop->data->num_windows; i++)
		mop->windows[i].enabled = false;

	return 0;
}

static const struct dev_pm_ops mstar_mop_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, mstar_mop_resume_noirq)
};

static const struct mstar_mop_data ssd20xd_mopg_data = {
	.num_windows = 16,
	.windows_start = 0x200,
	.window_len = 0x40,
};

/*
 * The sub plane's window block is at 0x80, not 0x20: the vendor reaches window 0
 * at 0xfd281080 (enable), 0xfd281084 (ctrl), 0xfd281088/8c and 0xfd281090/94
 * (luma and chroma address), 0xfd281098/9c/a0/a4 (the rectangle), 0xfd2810a8
 * (pitch), 0xfd2810ac/b0 (source size) and 0xfd2810b4/b8 (scale), which is the
 * same layout mopg's windows have, at base + 0x80. At 0x20 the driver was
 * writing windows into the global config registers instead.
 */
static const struct mstar_mop_data ssd20xd_mops_data = {
	.num_windows = 1,
	.windows_start = 0x80,
	.window_len = 0x40,
};

static const struct of_device_id mstar_mop_ids[] = {
	{
		.compatible = "sstar,ssd20xd-mopg",
		.data = &ssd20xd_mopg_data,
	},
	{
		.compatible = "sstar,ssd20xd-mops",
		.data = &ssd20xd_mops_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_mop_ids);

static struct platform_driver mstar_mop_driver = {
	.probe = mstar_mop_probe,
	.remove = mstar_mop_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_mop_ids,
		   .pm = pm_sleep_ptr(&mstar_mop_pm_ops),
	},
};

module_platform_driver(mstar_mop_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
