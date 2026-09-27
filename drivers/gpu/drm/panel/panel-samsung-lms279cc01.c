// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung LMS279CC01 2.8" 640x480 MIPI-DSI panel, as fitted to the Miyoo
 * Mini. A raw-command DSI panel: the init sequence (soft reset, MADCTL,
 * pixel format, window, brightness/CABC, tearing, exit sleep, display on)
 * was decoded from the vendor firmware's DSI command stream, and the timing
 * is exactly what the vendor programs, which is what locks the panel.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct lms279cc01 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	enum drm_panel_orientation orientation;
};

static inline struct lms279cc01 *to_lms279cc01(struct drm_panel *panel)
{
	return container_of(panel, struct lms279cc01, panel);
}

/*
 * RESX pulse the stock firmware issues before the DSI init: deasserted,
 * asserted for 20 ms, released, then the panel gets 120 ms to come up.
 * Without it the link is one-way and the panel stays blank.
 */
static void lms279cc01_reset(struct lms279cc01 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);
}

static int lms279cc01_prepare(struct drm_panel *panel)
{
	struct lms279cc01 *ctx = to_lms279cc01(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct device *dev = &ctx->dsi->dev;
	u8 mode = 0;
	int ret;

	if (ctx->reset_gpio)
		lms279cc01_reset(ctx);

	mipi_dsi_dcs_soft_reset_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_ADDRESS_MODE, 0x00);
	mipi_dsi_dcs_set_pixel_format_multi(&dsi_ctx, MIPI_DCS_PIXEL_FMT_24BIT << 4 |
					    MIPI_DCS_PIXEL_FMT_24BIT);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, 639);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, 479);
	/* manufacturer packet the vendor sends between the window and CABC */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x40, 0x30, 0x30, 0x0a, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS, 0x30);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_tear_scanline_multi(&dsi_ctx, 0x190);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	/*
	 * A DCS read makes the panel drive the link back, so - unlike the
	 * writes, whose completion only says the host finished sending - a
	 * sensible answer proves the panel is there and talking to us.
	 */
	ret = mipi_dsi_set_maximum_return_packet_size(ctx->dsi, 1);
	if (!ret)
		ret = mipi_dsi_dcs_get_power_mode(ctx->dsi, &mode);
	if (ret < 0)
		dev_warn(dev, "panel did not answer get_power_mode (%d): link is one-way\n", ret);
	else
		dev_info(dev, "panel power mode 0x%02x (sleep-out %s, display %s)\n", mode,
			 mode & MIPI_DSI_DCS_POWER_MODE_SLEEP ? "yes" : "no",
			 mode & MIPI_DSI_DCS_POWER_MODE_DISPLAY ? "on" : "off");

	return 0;
}

static int lms279cc01_unprepare(struct drm_panel *panel)
{
	struct lms279cc01 *ctx = to_lms279cc01(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	if (ctx->reset_gpio)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

/*
 * 640x480 with porches hfp=48 hsw=4 hbp=48 and vfp=10 vsw=4 vbp=10:
 * htotal 740, vtotal 504, 740 * 504 * 60 = 22.3776 MHz. The op2 timing
 * generator and the DSI stream are derived from this, so it has to be the
 * vendor's numbers exactly or the picture rolls.
 */
static const struct drm_display_mode lms279cc01_mode = {
	.clock = 22378,
	.hdisplay = 640,
	.hsync_start = 640 + 48,
	.hsync_end = 640 + 48 + 4,
	.htotal = 640 + 48 + 4 + 48,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 4,
	.vtotal = 480 + 10 + 4 + 10,
	.width_mm = 57,
	.height_mm = 44,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int lms279cc01_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &lms279cc01_mode);
}

static enum drm_panel_orientation lms279cc01_get_orientation(struct drm_panel *panel)
{
	struct lms279cc01 *ctx = to_lms279cc01(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs lms279cc01_panel_funcs = {
	.prepare = lms279cc01_prepare,
	.unprepare = lms279cc01_unprepare,
	.get_modes = lms279cc01_get_modes,
	.get_orientation = lms279cc01_get_orientation,
};

static int lms279cc01_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lms279cc01 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct lms279cc01, panel,
				   &lms279cc01_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get orientation\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* sync-pulse video, commands in LP mode, no EOT packets: the vendor setup */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_MODE_LPM;

	/* the host must be up in command mode before the init sequence */
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void lms279cc01_remove(struct mipi_dsi_device *dsi)
{
	struct lms279cc01 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id lms279cc01_of_match[] = {
	{ .compatible = "samsung,lms279cc01" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lms279cc01_of_match);

static struct mipi_dsi_driver lms279cc01_driver = {
	.driver = {
		.name = "panel-samsung-lms279cc01",
		.of_match_table = lms279cc01_of_match,
	},
	.probe = lms279cc01_probe,
	.remove = lms279cc01_remove,
};
module_mipi_dsi_driver(lms279cc01_driver);

MODULE_DESCRIPTION("DRM driver for the Samsung LMS279CC01 MIPI-DSI panel");
MODULE_LICENSE("GPL");
