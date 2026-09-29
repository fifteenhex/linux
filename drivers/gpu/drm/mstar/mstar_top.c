/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <drm/drm_crtc.h>
#include <drm/drm_vblank.h>
#include <linux/component.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>

#include "mstar_drm.h"
#include "mstar_top.h"

#define DRIVER_NAME "mstar-top"

#define REG_CTRL	0x0
#define REG_FIFORST	0xc0
#define REG_MACESRC	0x1c
#define REG_DSI		0xc8

static const struct reg_field reset_field = REG_FIELD(REG_CTRL, 8, 8);
static const struct reg_field irq_vsync_pos_flag_field = REG_FIELD(0x8, 3, 3);
static const struct reg_field irq_vsync_pos_mask_field = REG_FIELD(0xc, 3, 3);
static const struct reg_field fifo_rst_field = REG_FIELD(REG_FIFORST, 0, 0);

static const struct reg_field macesrc_field = REG_FIELD(REG_MACESRC, 0, 0);

static const struct reg_field disp2dsi_field = REG_FIELD(REG_DSI, 0, 0);

/*
 * The display "front" stage (0x1f224c00; the spec calls the bank the HDMI-TX
 * atop). The stock firmware writes exactly these registers before it brings
 * the D-PHY and DSI up, nothing else in the bank; without them the DSI path
 * stays dark. Replayed verbatim from an MMIO trace of the vendor u-boot.
 */
static const struct {
	u16 off;
	u16 val;
} mstar_top_front_init[] = {
	{ 0x064, 0x000a }, { 0x064, 0x110a }, { 0x068, 0x0004 },
	{ 0x0e0, 0x000a }, { 0x268, 0x0005 }, { 0x270, 0x004c },
};

/*
 * Route the finished DISP output into the MIPI-DSI host, sourcing the panel
 * from the mixed plane output rather than the pattern generator, with the
 * PNL formatter and the output FIFO reset in between - the vendor's
 * bring-up order for the displaytop. Called from the top's bind once the
 * op2 has said the output is DSI.
 */
void mstar_top_route_to_dsi(struct mstar_top *top)
{
	int i;

	if (top->front)
		for (i = 0; i < ARRAY_SIZE(mstar_top_front_init); i++)
			writew(mstar_top_front_init[i].val,
			       top->front + mstar_top_front_init[i].off);

	regmap_field_write(top->mace_src, 1);
	regmap_field_force_write(top->reset, 1);
	regmap_field_force_write(top->fifo_rst, 1);
	regmap_field_force_write(top->fifo_rst, 0);
	regmap_field_force_write(top->reset, 0);
	regmap_field_write(top->disp_to_dsi, 1);

	top->routed_to_dsi = true;
}

/*
 * Take the displaytop out of reset and park its vsync interrupt: masked, with
 * any latched flag cleared, so it stays quiet until the CRTC enables vblank.
 */
static void mstar_top_hw_init(struct mstar_top *top)
{
	regmap_field_force_write(top->reset, 1);
	mdelay(10);
	regmap_field_force_write(top->reset, 0);

	regmap_field_write(top->vsync_pos_mask, 1);
	regmap_field_force_write(top->vsync_pos_flag, 1);
}

static const struct regmap_config mstar_top_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static irqreturn_t mstar_top_irq(int irq, void *data)
{
	struct mstar_top *top = data;
	struct drm_crtc *crtc;
	unsigned long flags;

	regmap_field_force_write(top->vsync_pos_flag, 1);

	//printk("v irq 4\n");
	if (top->drm_device) {
		drm_for_each_crtc(crtc, top->drm_device) {
			drm_crtc_handle_vblank(crtc);
			//spin_lock_irqsave(&crtc->dev->event_lock, flags);
			//if (crtc->state && crtc->state->event) {
			//	printk("send event! %px\n",crtc->state->event);
			//	drm_crtc_send_vblank_event(crtc, crtc->state->event);
			//	drm_crtc_vblank_put(crtc);
			//	crtc->state->event = NULL;
			//}
			//spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		}
	}

	return IRQ_HANDLED;
}

static int mstar_top_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_top *top = dev_get_drvdata(dev);
	struct drm_device *drm_device = data;
	struct mstar_drv *drv = drm_device->dev_private;

	top->drm_device = drm_device;
	drv->top = top;

	/* the op2 binds before the top (ports order), so it already knows */
	if (drv->output_dsi)
		mstar_top_route_to_dsi(top);

	return 0;
}

static void mstar_top_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct drm_device *drm_device = data;
	struct mstar_drv *drv = drm_device->dev_private;

	drv->top = NULL;
}

static const struct component_ops mstar_top_ops = {
	.bind	= mstar_top_bind,
	.unbind	= mstar_top_unbind,
};

void mstar_top_enable_vblank(struct mstar_top *top)
{
	regmap_field_write(top->vsync_pos_mask, 0);
}

void mstar_top_disable_vblank(struct mstar_top *top)
{
	regmap_field_write(top->vsync_pos_mask, 1);
}

static int mstar_top_probe(struct platform_device *pdev)
{
	const struct mstar_top_data *match_data;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mstar_top *top;
	void __iomem *base;
	int irq, ret;

	top = devm_kzalloc(dev, sizeof(*top), GFP_KERNEL);
	if(!top){
		return -ENOMEM;
	}

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_top_regmap_config);
	if(IS_ERR(regmap))
		return PTR_ERR(regmap);

	/*
	 * Allocate the regmap fields before the IRQ handler can run: the vsync
	 * handler writes vsync_pos_flag, so it must exist first. (The shared
	 * handler could otherwise fire - from a co-registered device, or a
	 * latched vsync - the instant devm_request_irq() returns, dereferencing
	 * a NULL field and oopsing in hard-IRQ context.)
	 */
	top->reset = devm_regmap_field_alloc(dev, regmap, reset_field);
	if (IS_ERR(top->reset))
		return PTR_ERR(top->reset);
	top->vsync_pos_flag = devm_regmap_field_alloc(dev, regmap, irq_vsync_pos_flag_field);
	if (IS_ERR(top->vsync_pos_flag))
		return PTR_ERR(top->vsync_pos_flag);
	top->vsync_pos_mask = devm_regmap_field_alloc(dev, regmap, irq_vsync_pos_mask_field);
	if (IS_ERR(top->vsync_pos_mask))
		return PTR_ERR(top->vsync_pos_mask);
	top->mace_src = devm_regmap_field_alloc(dev, regmap, macesrc_field);
	if (IS_ERR(top->mace_src))
		return PTR_ERR(top->mace_src);
	top->fifo_rst = devm_regmap_field_alloc(dev, regmap, fifo_rst_field);
	if (IS_ERR(top->fifo_rst))
		return PTR_ERR(top->fifo_rst);
	top->disp_to_dsi = devm_regmap_field_alloc(dev, regmap, disp2dsi_field);
	if (IS_ERR(top->disp_to_dsi))
		return PTR_ERR(top->disp_to_dsi);

	/* the front stage bank is optional in the device tree */
	top->front = devm_platform_ioremap_resource_byname(pdev, "front");
	if (IS_ERR(top->front))
		top->front = NULL;
	dev_dbg(dev, "front bank %s\n", top->front ? "mapped" : "absent");

	/* done before the handler is registered, so a pending vsync can't fire it */
	mstar_top_hw_init(top);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	ret = devm_request_irq(&pdev->dev, irq, mstar_top_irq, IRQF_SHARED,
		dev_name(&pdev->dev), top);
	if (ret)
		return ret;

	//
	struct clk *clk;
	clk = devm_clk_get(dev, "sc_pixel");
	if (IS_ERR(clk)) {
		printk("failed to get clock\n");
		return PTR_ERR(clk);
	}

	clk_prepare_enable(clk);

	clk = devm_clk_get(dev, "disp_432");
	clk_prepare_enable(clk);

	clk = devm_clk_get(dev, "disp_216");
	clk_prepare_enable(clk);

	clk = devm_clk_get(dev, "hdmi");
	clk_prepare_enable(clk);
	//

	dev_set_drvdata(dev, top);

	return component_add(&pdev->dev, &mstar_top_ops);
}

static void mstar_top_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_top_ops);
}

/*
 * Suspend to RAM ends in a soft reset of the whole SoC, so the displaytop comes
 * back held in reset with the DISP output unrouted and the front stage blank -
 * all of it set up once at probe/bind time. Redo that here, in the noirq phase
 * so it is done before the DRM master's resume redoes the mode set; without it
 * nothing reaches the DSI host and the CRTC produces no vsync interrupt.
 */
static int mstar_top_resume_noirq(struct device *dev)
{
	struct mstar_top *top = dev_get_drvdata(dev);

	mstar_top_hw_init(top);
	if (top->routed_to_dsi)
		mstar_top_route_to_dsi(top);

	return 0;
}

static const struct dev_pm_ops mstar_top_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, mstar_top_resume_noirq)
};

static const struct of_device_id mstar_top_dt_ids[] = {
	{
		.compatible = "sstar,ssd20xd-display-top",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_top_dt_ids);

static struct platform_driver mstar_top_driver = {
	.probe = mstar_top_probe,
	.remove = mstar_top_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_top_dt_ids,
		   .pm = pm_sleep_ptr(&mstar_top_pm_ops),
	},
};
module_platform_driver(mstar_top_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
