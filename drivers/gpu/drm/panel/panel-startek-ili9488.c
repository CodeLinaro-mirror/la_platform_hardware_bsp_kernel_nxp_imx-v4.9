#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pwm.h>
#include <uapi/linux/media-bus-format.h>
#include <video/mipi_display.h>
#include <video/videomode.h>

struct startek_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	struct pwm_device *bl_pwm;
};

static const struct display_timing startek_default_timing = {
		.pixelclock = {0, 24000000, 0},
		.hactive = {0, 320, 0},
		.hfront_porch = {0, 170, 0}, // ref spec p55 for porch
		.hsync_len = {0, 50, 0},
		.hback_porch = {0, 50, 0},
		.vactive = {0, 480, 0},
		.vfront_porch = {0, 2, 0},
		.vsync_len = {0, 1, 0},
		.vback_porch = {0, 1, 0},
		.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
						 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

#define MAX_PARA_NUM 16
struct dcs_command {
	unsigned char cmd;
	unsigned char para_size;
	unsigned char para[MAX_PARA_NUM];
	unsigned int sleep;
};

static const struct dcs_command g_startek_init[] = {
		// PGAMCTRL
		{.cmd = 0xe0,
		 .para_size = 15,
		 .para = {0x00, 0x04, 0x0e, 0x08, 0x17, 0x0a, 0x40, 0x79, 0x4d, 0x07, 0x0e,
							0x0a, 0x1a, 0x1d, 0x0f}},

		// NGAMCTRL
		{.cmd = 0xe1,
		 .para_size = 15,
		 .para = {0x00, 0x1b, 0x1f, 0x02, 0x10, 0x05, 0x32, 0x34, 0x43, 0x02, 0x0a,
							0x09, 0x33, 0x37, 0x0f}},

		// Power Control 1
		{.cmd = 0xc0, .para_size = 2, .para = {0x18, 0x16}},

		// Power Control 2
		{.cmd = 0xc1, .para_size = 1, .para = {0x41}},

		// VCOM Control
		{.cmd = 0xc5, .para_size = 3, .para = {0x00, 0x1e, 0x80}},

		// Interface Mode Control
		{.cmd = 0x3a, .para_size = 1, .para = {0x77}},

		// Frame rate 70HZ
		{.cmd = 0xb1, .para_size = 1, .para = {0xb0}},

		// Display Inversion Control
		{.cmd = 0xb4, .para_size = 1, .para = {0x02}},

		// Blanking Porch Control
		{.cmd = 0xb5, .para_size = 4, .para = {0x02, 0x02, 0xaa, 0x64}},

		// Set Image Function
		{.cmd = 0xe9, .para_size = 1, .para = {0x01}},

		// Adjust Control 3
		{.cmd = 0xf7, .para_size = 4, .para = {0xa9, 0x51, 0x2c, 0x82}},

		// Display Function Control
		{.cmd = 0xb6, .para_size = 3, .para = {0x02, 0x02, 0x3b}},

		// Column Address Set
		{.cmd = 0x2a, .para_size = 4, .para = {0x00, 0x00, 0x01, 0x3f}},

		// Page Address Set
		{.cmd = 0x2b, .para_size = 4, .para = {0x00, 0x00, 0x01, 0xdf}},

		// Entry Mode Set
		{.cmd = 0xb7, .para_size = 1, .para = {0xc6}},

		// Interface Mode Control
		{.cmd = 0xb0, .para_size = 1, .para = {0x00}},

};

// Seems 0x36 only takes effect after exit sleep.
// And if init all registers after exit sleep, other issue.
// So we seperate 2 init phases.
static const struct dcs_command g_startek_later_init[] = {
		// Memory Access Control
		{.cmd = 0x36, .para_size = 1, .para = {0x48}},
};

static int startek_init(struct mipi_dsi_device *dsi,
												struct dcs_command *init_command, int num) {
	int i;
	int ret;

	dev_info(&dsi->dev, "%s(), command num %d\n", __func__, num);
	for (i = 0; i < num; i++) {
		struct dcs_command *cmd = &init_command[i];

		ret = mipi_dsi_dcs_write(dsi, cmd->cmd, cmd->para, cmd->para_size);
		if (ret < 0) {
			dev_err(&dsi->dev, "%s(), command 0x%x, ret %d\n", __func__, cmd->cmd,
							ret);
			return ret;
		}

		if (cmd->sleep)
			msleep(cmd->sleep);
	}

	return 0;
}

static int startek_panel_get_modes(struct drm_panel *panel) {
	struct startek_panel *startek =
			container_of(panel, struct startek_panel, panel);
	struct mipi_dsi_device *dsi = startek->dsi;
	int ret;
	struct videomode vm;
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	struct drm_display_mode *mode;
	mode = drm_mode_create(panel->connector->dev);
	if (!mode) {
		dev_err(&dsi->dev, "Failed drm_mode_create");
		return 0;
	}

	videomode_from_timing(&startek_default_timing, &vm);

	drm_display_mode_from_videomode(&vm, mode);
	mode->width_mm = 49;
	mode->height_mm = 74;
	panel->connector->display_info.height_mm = mode->height_mm;
	panel->connector->display_info.width_mm = mode->width_mm;
	panel->connector->display_info.bus_flags =
			DRM_BUS_FLAG_PIXDATA_POSEDGE | DRM_BUS_FLAG_DE_HIGH;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	ret = drm_display_info_set_bus_formats(&panel->connector->display_info,
																				 &bus_format, 1);

	if (ret) {
		return 0;
	}

	drm_mode_probed_add(panel->connector, mode);

	return 1;
}

static int startek_panel_prepare(struct drm_panel *panel) {
	int ret = 0;
	struct startek_panel *startek =
			container_of(panel, struct startek_panel, panel);
	struct mipi_dsi_device *dsi = startek->dsi;
	uint8_t ctrl = 0x2C;
	uint8_t powersave = 0x0;
	uint16_t min_bright = 0x80;

	gpiod_set_value(startek->reset, 1);
	msleep(1);
	gpiod_set_value(startek->reset, 0);
	msleep(10);
	gpiod_set_value(startek->reset, 1);
	msleep(120);

	ret = startek_init(dsi, g_startek_init, ARRAY_SIZE(g_startek_init));
	if (ret < 0) {
		dev_err(&dsi->dev, "%s(), startek_init failed, ret %d\n", __func__, ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_display_brightness(dsi, 0x0ff);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to set brightness: %d", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, &ctrl,
													 sizeof(ctrl));
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to set control: %d", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_POWER_SAVE, &powersave,
													 sizeof(powersave));
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to set powersave: %d", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS, &min_bright,
													 sizeof(min_bright));
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to set min_bright: %d", ret);
		return ret;
	}

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to exit sleep mode: %d", ret);
		return ret;
	}
	msleep(120);

	startek_init(dsi, g_startek_later_init, ARRAY_SIZE(g_startek_later_init));

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to turn on display: %d", ret);
		return ret;
	}

	dev_info(&dsi->dev, "prepare ok\n");

	return 0;
}

static int startek_panel_unprepare(struct drm_panel *panel) {
	struct startek_panel *startek =
			container_of(panel, struct startek_panel, panel);
	struct mipi_dsi_device *dsi = startek->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Could not set display off: %d", ret);
		return ret;
	}
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Could not enter sleep mode: %d", ret);
		return ret;
	}

	return 0;
}

static int startek_panel_enable(struct drm_panel *panel) {
	struct startek_panel *startek =
			container_of(panel, struct startek_panel, panel);

	startek->backlight->props.power = FB_BLANK_UNBLANK;
	backlight_update_status(startek->backlight);

	return 0;
}

static int startek_panel_disable(struct drm_panel *panel) {
	struct startek_panel *startek =
			container_of(panel, struct startek_panel, panel);

	startek->backlight->props.power = FB_BLANK_POWERDOWN;
	backlight_update_status(startek->backlight);

	return 0;
}

static int startek_bl_get_brightness(struct backlight_device *bl) {
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct startek_panel *startek =
			container_of(bl, struct startek_panel, backlight);

	int ret;

	u16 brightness = 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0) {
		dev_err(&dsi->dev, "Could not get brightness: %d", ret);
		return ret;
	}
	bl->props.brightness = brightness;

	return brightness & 0xff;
}

static int startek_bl_update_status(struct backlight_device *bl) {
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct startek_panel *startek =
			container_of(bl, struct startek_panel, backlight);
	int ret = 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);

	return ret;
}

static const struct drm_panel_funcs startek_panel_funcs = {
		.get_modes = startek_panel_get_modes,
		.enable = startek_panel_enable,
		.disable = startek_panel_disable,
		.prepare = startek_panel_prepare,
		.unprepare = startek_panel_unprepare,
};

static struct backlight_ops startek_bl_ops = {
		.update_status = startek_bl_update_status,
		.get_brightness = startek_bl_get_brightness,
};

static int startek_panel_probe(struct mipi_dsi_device *dsi) {
	struct startek_panel *panel;
	int ret;
	struct backlight_properties bl_props = {
			.type = BACKLIGHT_RAW, .brightness = 0xff, .max_brightness = 0xff,
	};

	panel = devm_kzalloc(&dsi->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;
	panel->dsi = dsi;

	mipi_dsi_set_drvdata(dsi, panel);

	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
										MIPI_DSI_CLOCK_NON_CONTINUOUS;
	ret = of_property_read_u32(dsi->dev.of_node, "dsi-lanes", &dsi->lanes);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to get dsi-lanes property: %d", ret);
		return ret;
	}

	panel->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset)) {
		ret = PTR_ERR(panel->reset);
		dev_err(&dsi->dev, "Failed to get reset gpio: %d\n", ret);
		return ret;
	}

	panel->backlight =
			devm_backlight_device_register(&dsi->dev, dev_name(&dsi->dev), &dsi->dev,
																		 dsi, &startek_bl_ops, &bl_props);
	if (IS_ERR(panel->backlight)) {
		ret = PTR_ERR(panel->backlight);
		dev_err(&dsi->dev, "Failed to register backlight: %d\n", ret);
		return ret;
	}

	drm_panel_init(&panel->panel);
	panel->panel.funcs = &startek_panel_funcs;
	panel->panel.dev = &dsi->dev;

	ret = drm_panel_add(&panel->panel);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to add panel: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Unable to mipi_dsi_attach! %d\n", ret);
		return ret;
	}

	panel->bl_pwm = devm_pwm_get(&dsi->dev, NULL);
	if (panel->bl_pwm == NULL) {
		ret = PTR_ERR(panel->bl_pwm);
		dev_err(&dsi->dev, "Failed to get backlight pwm: %d\n", ret);
		return ret;
	}

	pwm_config(panel->bl_pwm, 300000, 1000000);
	pwm_enable(panel->bl_pwm);

	struct startek_panel *startek = panel;

	return ret;
}

static int startek_panel_remove(struct mipi_dsi_device *dsi) {
	struct startek_panel *panel = mipi_dsi_get_drvdata(dsi);

	gpiod_set_value(panel->reset, 1);

	return 0;
}

static const struct of_device_id startek_of_match[] = {
		{
				.compatible = "startek,ili9488",
		},
		{}};
MODULE_DEVICE_TABLE(of, startek_of_match);

static struct mipi_dsi_driver startek_panel_driver = {
		.driver =
				{
						.name = "panel-startek-ili9488", .of_match_table = startek_of_match,
				},
		.probe = startek_panel_probe,
		.remove = startek_panel_remove,
};
module_mipi_dsi_driver(startek_panel_driver);

MODULE_AUTHOR("Fang Hui <hui.fang@nxp.com>");
MODULE_DESCRIPTION("Startek Display panel");
MODULE_LICENSE("GPL v2");
