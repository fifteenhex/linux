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

#define DRIVER_NAME "mstar-top"

struct mstar_top {
	struct drm_device *drm_device;
	struct regmap_field *vsync_pos_flag;
	struct regmap_field *vsync_pos_mask;
};

static const struct reg_field irq_vsync_pos_flag_field = REG_FIELD(0x8, 3, 3);
static const struct reg_field irq_vsync_pos_mask_field = REG_FIELD(0xc, 3, 3);

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

#if 0
	if (top->drm_device && drm_dev_has_vblank(top->drm_device)) {
		drm_for_each_crtc(crtc, top->drm_device){
		drm_crtc_handle_vblank(crtc);
			spin_lock_irqsave(&crtc->dev->event_lock, flags);
			if (crtc->state && crtc->state->event) {
				printk("send event! %px\n",crtc->state->event);
				drm_crtc_send_vblank_event(crtc, crtc->state->event);
				drm_crtc_vblank_put(crtc);
				crtc->state->event = NULL;
			}
			spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		}
	}
#endif

	return IRQ_HANDLED;
}

static int mstar_top_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_top *top = dev_get_drvdata(dev);
	struct drm_device *drm_device = data;

	top->drm_device = drm_device;

	return 0;
}

static void mstar_top_unbind(struct device *dev, struct device *master,
			    void *data)
{
}

static const struct component_ops mstar_top_ops = {
	.bind	= mstar_top_bind,
	.unbind	= mstar_top_unbind,
};

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

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	ret = devm_request_irq(&pdev->dev, irq, mstar_top_irq, IRQF_SHARED,
		dev_name(&pdev->dev), top);
	if (ret)
		return ret;

	top->vsync_pos_flag = regmap_field_alloc(regmap, irq_vsync_pos_flag_field);
	top->vsync_pos_mask = regmap_field_alloc(regmap, irq_vsync_pos_mask_field);

	dev_set_drvdata(dev, top);

	regmap_field_write(top->vsync_pos_mask, 0);

	return component_add(&pdev->dev, &mstar_top_ops);
}

static int mstar_top_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_top_ops);

	return 0;
}

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
	},
};
module_platform_driver(mstar_top_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
