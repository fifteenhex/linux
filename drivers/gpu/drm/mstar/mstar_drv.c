/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Based on the sunxi DRM driver
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_drv.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "mstar_drm.h"
#include "mstar_framebuffer.h"

#define DRIVER_NAME "mstar-drm"

DEFINE_DRM_GEM_DMA_FOPS(mstar_drv_fops);

static const struct drm_driver mstar_drv_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,

	.fops = &mstar_drv_fops,
	.name = "mstar-drm",
	.desc = "MStar DRM driver",
	.major = 1,
	.minor = 0,

	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static const struct drm_mode_config_funcs drv_mode_config_funcs = {
	/*
	 * Without an fb_create hook, userspace DRM_IOCTL_MODE_ADDFB[2] returns
	 * -EINVAL, so no userspace client (a GE demo, a KMS app, ...) can create
	 * a framebuffer to scan out - only the in-kernel fbdev worked. Use the
	 * standard GEM helper (buffers are drm_gem_dma objects).
	 */
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int mstar_drv_bind(struct device *dev)
{
	struct drm_device *drm;
	struct mstar_drv *drv;
	int ret;

	drm = drm_dev_alloc(&mstar_drv_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		ret = -ENOMEM;
		goto free_drm;
	}

	dev_set_drvdata(dev, drm);
	drm->dev_private = drv;

	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV) {
		dev_err(drm->dev, "Couldn't claim our memory region\n");
		goto free_drm;
	}

	drm_mode_config_init(drm);
	//drm->mode_config.allow_fb_modifiers = true;
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 8198;
	drm->mode_config.max_height = 8198;
	drm->mode_config.funcs = &drv_mode_config_funcs;

	ret = component_bind_all(drm->dev, drm);
	if (ret) {
		dev_err(drm->dev, "Couldn't bind all pipelines components\n");
		goto cleanup_mode_config;
	}

	/* drm_vblank_init calls kcalloc, which can fail */
	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret)
		goto cleanup_mode_config;

	//drm->irq_enabled = true;

	/* Remove early framebuffers (ie. simplefb) */
	//remove_conflicting_framebuffers(NULL, "xx", false);

	mstar_framebuffer_init(drm);

	/* Enable connectors polling */
	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto finish_poll;

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);

	return 0;

finish_poll:
	drm_kms_helper_poll_fini(drm);
cleanup_mode_config:
	drm_mode_config_cleanup(drm);
	of_reserved_mem_device_release(dev);

free_drm:
	drm_dev_put(drm);

	return ret;
}

static void mstar_drv_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_dev_unregister(drm);
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);
	drm_mode_config_cleanup(drm);

	component_unbind_all(dev, NULL);
	of_reserved_mem_device_release(dev);

	drm_dev_put(drm);
}

static const struct component_master_ops mstar_drv_master_ops = {
	.bind	= mstar_drv_bind,
	.unbind	= mstar_drv_unbind,
};

static int compare_of(struct device *dev, void *data)
{
	struct device_node *np = data;

	return dev->of_node == np;
}

static int mstar_drm_probe(struct platform_device *pdev)
{
	return drm_of_component_probe(&pdev->dev, compare_of, &mstar_drv_master_ops);
}

static void mstar_drm_remove(struct platform_device *pdev)
{

}

static const struct of_device_id mstar_drm_dt_ids[] = {
	{ .compatible = "sstar,ssd20xd-drm" },
	{},
};
MODULE_DEVICE_TABLE(of, mstar_drm_dt_ids);

/*
 * Suspend to RAM ends in a soft reset, so the whole display pipeline has to be
 * brought up from scratch on resume. The helpers redo the mode set, which takes
 * care of everything the atomic enable paths program. What they do not touch is
 * the setup each block only does at probe or bind time; the components redo
 * that themselves in their noirq resume, which runs before this one.
 */
static int mstar_drv_suspend(struct device *dev)
{
	return drm_mode_config_helper_suspend(dev_get_drvdata(dev));
}

static int mstar_drv_resume(struct device *dev)
{
	return drm_mode_config_helper_resume(dev_get_drvdata(dev));
}

static DEFINE_SIMPLE_DEV_PM_OPS(mstar_drm_pm_ops, mstar_drv_suspend, mstar_drv_resume);

static struct platform_driver mstar_drm_driver = {
	.probe = mstar_drm_probe,
	.remove = mstar_drm_remove,
	.driver = {
		.pm = pm_sleep_ptr(&mstar_drm_pm_ops),
		   .name = DRIVER_NAME,
		   .of_match_table = mstar_drm_dt_ids,
	},
};

module_platform_driver(mstar_drm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mstar DRM Driver");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
