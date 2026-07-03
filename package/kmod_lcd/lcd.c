// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for XL1.9 ST7789V panel (170x320 SPI + DC GPIO)
 *
 * Uses 320x170 FB + software transpose -> 170x320 display (MADCTL=0x00)
 * Console renders landscape text, driver transposes for portrait panel.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_gem_framebuffer_helper.h>

struct st7789v_cfg {
	const struct drm_display_mode mode;
	unsigned int left_offset;
	unsigned int top_offset;
};

struct st7789v_priv {
	struct mipi_dbi_dev dbidev;
	const struct st7789v_cfg *cfg;
	u8 *tx_buf_t;	/* transposed buffer (same size as tx_buf) */
};

static void st7789v_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	pr_err("ST7789V init (landscape transpose)...\n");

	/* Hardware reset */
	if (dbi->reset) {
		gpiod_set_raw_value_cansleep(dbi->reset, 0);
		msleep(100);
		gpiod_set_raw_value_cansleep(dbi->reset, 1);
		msleep(100);
	} else {
		msleep(200);
	}

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x05);
	mipi_dbi_command(dbi, 0xC5, 0x1A);

	/* MADCTL=0x40 (MX=1): mirror X to fix panel scan direction */
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, 0x40);

	mipi_dbi_command(dbi, 0xB2, 0x05, 0x05, 0x00, 0x33, 0x33);
	mipi_dbi_command(dbi, 0xB7, 0x05);
	mipi_dbi_command(dbi, 0xBB, 0x3F);
	mipi_dbi_command(dbi, 0xC0, 0x2C);
	mipi_dbi_command(dbi, 0xC2, 0x01);
	mipi_dbi_command(dbi, 0xC3, 0x0F);
	mipi_dbi_command(dbi, 0xC4, 0x20);
	mipi_dbi_command(dbi, 0xC6, 0x01);
	mipi_dbi_command(dbi, 0xD0, 0xA4, 0xA1);
	mipi_dbi_command(dbi, 0xE8, 0x03);
	mipi_dbi_command(dbi, 0xE9, 0x09, 0x09, 0x08);

	mipi_dbi_command(dbi, 0xE0,
			 0xD0, 0x05, 0x09, 0x09, 0x08,
			 0x14, 0x28, 0x33, 0x3F, 0x07,
			 0x13, 0x14, 0x28, 0x30);

	mipi_dbi_command(dbi, 0xE1,
			 0xD0, 0x05, 0x09, 0x09, 0x08,
			 0x03, 0x24, 0x32, 0x32, 0x3B,
			 0x14, 0x13, 0x28, 0x2F);

	mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);

	if (dbi->reset) {
		gpiod_set_raw_value_cansleep(dbi->reset, 1);
		pr_err("ST7789V init done\n");
	}
	drm_dev_exit(idx);
}

/* Software transpose: 320x170 FB -> 170x320 for display */
static void st7789v_transpose(u8 *dst, u8 *src, unsigned int fw, unsigned int fh)
{
	unsigned int x, y;
	u16 *s = (u16 *)src;
	u16 *d = (u16 *)dst;

	for (y = 0; y < fh; y++)
		for (x = 0; x < fw; x++)
			d[x * fh + y] = s[y * fw + x];
}

static void st7789v_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect *clip = &state->dst;
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct st7789v_priv *priv = container_of(dbidev, struct st7789v_priv, dbidev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	struct drm_framebuffer *fb = state->fb;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
	unsigned int fw, fh;
	int idx, ret;

	if (!fb || !state->visible)
		return;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret)
		goto out;

	/* Convert FB to RGB565 -> tx_buf (320x170) */
	ret = mipi_dbi_buf_copy(dbidev->tx_buf, fb, clip, dbi->swap_bytes);
	if (ret)
		goto out_unmap;

	fw = clip->x2 - clip->x1;  /* 320 */
	fh = clip->y2 - clip->y1;  /* 170 */

	/* Transpose 320x170 -> 170x320 for panel */
	st7789v_transpose(priv->tx_buf_t, (u8 *)dbidev->tx_buf, fw, fh);

	/* Set window: CASET(X)=35..204, RASET(Y)=0..(fw-1)=319 */
	{
		u16 xs = dbidev->left_offset;
		u16 xe = fh - 1 + dbidev->left_offset;
		u16 ys = dbidev->top_offset;
		u16 ye = fw - 1 + dbidev->top_offset;
		mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS,
				 (xs >> 8) & 0xFF, xs & 0xFF,
				 (xe >> 8) & 0xFF, xe & 0xFF);
		mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS,
				 (ys >> 8) & 0xFF, ys & 0xFF,
				 (ye >> 8) & 0xFF, ye & 0xFF);
	}

	/* Write transposed data */
	mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START,
			     priv->tx_buf_t, fw * fh * 2);

out_unmap:
	drm_gem_fb_vunmap(fb, map);
out:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs st7789v_pipe_funcs = {
	.mode_valid	= mipi_dbi_pipe_mode_valid,
	.enable		= st7789v_pipe_enable,
	.disable	= mipi_dbi_pipe_disable,
	.update		= st7789v_pipe_update,
};

/* FB: 320x170 landscape, transpose -> 170x320 for panel */
static const struct st7789v_cfg st7789v_default_cfg = {
	.mode		= { DRM_SIMPLE_MODE(320, 170, 28, 54) },
	.left_offset	= 35,
	.top_offset	= 0,
};

DEFINE_DRM_GEM_DMA_FOPS(st7789v_fops);

static const struct drm_driver st7789v_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &st7789v_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "st7789v",
	.desc			= "Sitronix ST7789V",
	.date			= "20250124",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7789v_of_match[] = {
	{ .compatible = "sitronix,st7789v", .data = &st7789v_default_cfg },
	{ },
};
MODULE_DEVICE_TABLE(of, st7789v_of_match);

static const struct spi_device_id st7789v_id[] = {
	{ "st7789v", (uintptr_t)&st7789v_default_cfg },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7789v_id);

static int st7789v_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	const struct st7789v_cfg *cfg;
	struct mipi_dbi_dev *dbidev;
	struct st7789v_priv *priv;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	cfg = device_get_match_data(dev);
	if (!cfg)
		cfg = (void *)spi_get_device_id(spi)->driver_data;

	priv = devm_drm_dev_alloc(dev, &st7789v_driver,
				  struct st7789v_priv, dbidev.drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	dbidev = &priv->dbidev;
	priv->cfg = cfg;

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset),
				     "Failed to get GPIO 'reset'\n");

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	dbi->swap_bytes = true;

	dbidev->left_offset = cfg->left_offset;
	dbidev->top_offset = cfg->top_offset;

	/* Alloc transpose buffer */
	priv->tx_buf_t = devm_kzalloc(dev, 320 * 170 * 2, GFP_KERNEL);
	if (!priv->tx_buf_t)
		return -ENOMEM;

	ret = mipi_dbi_dev_init(dbidev, &st7789v_pipe_funcs, &cfg->mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 0);
	drm_fb_helper_hotplug_event(drm->fb_helper);

	if (dbi->reset) {
		gpiod_set_raw_value_cansleep(dbi->reset, 1);
		msleep(20);
	}

	pr_err("ST7789V landscape transpose probe done\n");
	return 0;
}

static void st7789v_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void st7789v_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver st7789v_spi_driver = {
	.driver = {
		.name = "st7789v",
		.of_match_table = st7789v_of_match,
	},
	.id_table = st7789v_id,
	.probe = st7789v_probe,
	.remove = st7789v_remove,
	.shutdown = st7789v_shutdown,
};
module_spi_driver(st7789v_spi_driver);

MODULE_DESCRIPTION("ST7789V 320x170 landscape DRM driver");
MODULE_LICENSE("GPL");
