// SPDX-License-Identifier: GPL-2.0-or-later

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "mstar_framebuffer.h"

static int mstar_framebuffer_atomic_check(struct drm_device *dev,
				 struct drm_atomic_state *state)
{
	/*int ret;

	ret = drm_atomic_helper_check_modeset(dev, state);
	if (ret)
		return ret;

	ret = drm_atomic_normalize_zpos(dev, state);
	if (ret)
		return ret;*/

	return drm_atomic_helper_check_planes(dev, state);
}

static const struct drm_mode_config_funcs mstar_framebuffer_mode_config_funcs = {
	.atomic_check		= mstar_framebuffer_atomic_check,
	.atomic_commit		= drm_atomic_helper_commit,
	.fb_create		= drm_gem_fb_create,
};

static struct drm_mode_config_helper_funcs mstar_framebuffer_mode_config_helpers = {
	.atomic_commit_tail	= drm_atomic_helper_commit_tail_rpm,
};

void mstar_framebuffer_init(struct drm_device *drm)
{
	drm_mode_config_reset(drm);

	drm->mode_config.max_width = 8192;
	drm->mode_config.max_height = 8192;

	drm->mode_config.funcs = &mstar_framebuffer_mode_config_funcs;
	drm->mode_config.helper_private = &mstar_framebuffer_mode_config_helpers;
}
