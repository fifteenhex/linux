#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME "mstar-op2"

struct mstar_op2 {
	struct drm_crtc drm_crtc;
};

static const struct regmap_config mstar_op2_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static const struct drm_crtc_funcs mstar_op2_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static int mstar_op2_mode_set(struct drm_crtc *crtc, struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode, int x, int y,
		struct drm_framebuffer *old_fb)
{
	return 0;
}

static const struct drm_crtc_helper_funcs mstar_op2_helper_funcs = {
	.mode_set = mstar_op2_mode_set,
};

static int mstar_op2_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct mstar_op2 *op2 = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	struct drm_plane *plane = NULL, *primary = NULL, *cursor = NULL;
	int ret;

	drm_for_each_plane(plane, drm){
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			primary = plane;
		else if (plane->type == DRM_PLANE_TYPE_CURSOR)
			cursor = plane;
	}

	if (primary == NULL)
		return -ENODEV;

	ret = drm_crtc_init_with_planes(drm,
					&op2->drm_crtc,
					primary,
					cursor,
					&mstar_op2_crtc_funcs,
					"op2");
	if(ret)
		return ret;

	drm_crtc_helper_add(&op2->drm_crtc, &mstar_op2_helper_funcs);

	/* set the port so the encoder can find us */
	op2->drm_crtc.port = of_graph_get_port_by_id(dev->of_node, 0);

	return 0;
}

static void mstar_op2_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct mstar_op2 *op2 = dev_get_drvdata(dev);

	drm_crtc_cleanup(&op2->drm_crtc);
}

static const struct component_ops mstar_op2_component_ops = {
	.bind	= mstar_op2_bind,
	.unbind	= mstar_op2_unbind,
};

static int mstar_op2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_op2 *op2;
	struct regmap *regmap;
	void __iomem *base;

	op2 = devm_kzalloc(dev, sizeof(*op2), GFP_KERNEL);
	if (!op2)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_op2_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	dev_set_drvdata(dev, op2);

	return component_add(&pdev->dev, &mstar_op2_component_ops);
}

static int mstar_op2_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_op2_component_ops);
	return 0;
}

static const struct of_device_id mstar_op2_ids[] = {
	{
		.compatible = "sstar,ssd20xd-op2",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_op2_ids);

static struct platform_driver mstar_op2_driver = {
	.probe = mstar_op2_probe,
	.remove = mstar_op2_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_op2_ids,
	},
};
module_platform_driver(mstar_op2_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
