// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <linux/dma-buf.h>
#include <linux/printk.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "fl2000.h"

#define DRM_DRIVER_NAME "fl2000_drm"
#define DRM_DRIVER_DESC "USB-VGA/HDMI"
#define DRM_DRIVER_DATE "20181001"

#define DRM_DRIVER_MAJOR 1
#define DRM_DRIVER_MINOR 0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL2000_MAX_WIDTH 4000
#define FL2000_MAX_HEIGHT 4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT 100

#define FL2000_BULK_BW_HIGH_SPEED                                              \
	(480000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED                                             \
	(5000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS                                        \
	(10000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)

static u32 fl2000_get_bytes_pix(enum usb_device_speed speed, u32 pixclock)
{
	int bytes_pix;
	u64 max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED_PLUS;
		break;
	default:
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	bytes_pix = max_bw / pixclock;
	if (bytes_pix > 3) {
		bytes_pix = 3;
	}
	return bytes_pix;
}

static struct drm_gem_object *fl2000_gem_prime_import(struct drm_device *dev,
						      struct dma_buf *dma_buf)
{
	struct fl2000 *fl2000_dev = container_of(dev, struct fl2000, drm);

	if (!fl2000_dev->dmadev)
		return ERR_PTR(-ENODEV);

	return drm_gem_prime_import_dev(dev, dma_buf, fl2000_dev->dmadev);
}

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

const struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,

	.fops = &fl2000_drm_driver_fops,

	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_prime_import = fl2000_gem_prime_import,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.date = DRM_DRIVER_DATE,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = (u64)vco_clk * FL2000_PLL_PRECISION / divisor;
	u64 pll_clk_err;

	/* Not using abs() here to avoid possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return pll_clk_err / (clock_mil / FL2000_PLL_PRECISION);
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk,
					 u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,
		17,  18,  19,  20,  21,	 22,  23,  24,	25,  26,  27,  28,  29,
		30,  31,  32,  33,  34,	 35,  36,  37,	38,  39,  40,  41,  42,
		43,  44,  45,  46,  47,	 48,  49,  50,	51,  52,  53,  54,  55,
		56,  57,  58,  59,  60,	 61,  62,  63,	64,  65,  66,  67,  68,
		69,  70,  71,  72,  73,	 74,  75,  76,	77,  78,  79,  80,  81,
		82,  83,  84,  85,  86,	 87,  88,  89,	90,  91,  92,  93,  94,
		95,  96,  97,  98,  99,	 100, 101, 102, 103, 104, 105, 106, 107,
		108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
		121, 122, 123, 124, 125, 126, 127, 128
	};
	unsigned int divisor_idx;
	u32 best_divisor = 0;

	/* Iterate over array */
	for (divisor_idx = 0; divisor_idx < ARRAY_SIZE(divisor_arr);
	     divisor_idx++) {
		u32 divisor = divisor_arr[divisor_idx];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll,
			   u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u32 prescaler;
	u32 multiplier;
	u64 min_ppm_err = (u64)(-1);

	for (prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (multiplier = 1; multiplier <= multiplier_max;
		     multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN ||
			    vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk,
							 &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			pll->function = vco_clk < 125000000 ? 0 :
					vco_clk < 250000000 ? 1 :
					vco_clk < 500000000 ? 2 :
								    3;
			*clock_calculated = vco_clk / divisor;
		}

	/* No exact PLL settings found for requested clock */
	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode,
			    struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;
	int s, m;
	int d = 0;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -1;

	/* Try to match pixel clock slightly adjusting htotal value */
	for (m = 0, s = 1; m <= max_h_adjustment * 2; m++, s = -s) {
		/* 0, -1, 1, -2, 2, -3, 3, -3, 4, -4, 5, -5, ... */
		d += m * s;

		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur. Assume all this was checked before
		 */
		clock_mil_adjusted =
			clock_mil * (mode->htotal + d) / mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll,
					  &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}
			pll->min_ppm_err = ppm_err;
			return 0;
		}
	}

	/* Cannot find PLL configuration that satisfy requirements */
	return -1;
}

static enum drm_mode_status
fl2000_display_mode_valid(struct drm_simple_display_pipe *pipe,
			  const struct drm_display_mode *mode)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;
	struct fl2000 *fl2000_dev = drm->dev_private;
	struct usb_device *usb_dev = fl2000_dev->usb_dev;

	/* Get PLL configuration and check if mode adjustments needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll))
		return MODE_BAD;

	if (!fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode.clock * 1000))
		return MODE_BAD;

	return MODE_OK;
}

static void fl2000_output_mode_set(struct fl2000 *fl2000_dev,
				   struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct usb_device *usb_dev = fl2000_dev->usb_dev;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	u32 bytes_pix;

	/* Get PLL configuration and cehc if mode adjustments needed */
	if (fl2000_mode_calc(mode, adjusted_mode, &pll))
		return;

	/* Check how many bytes per pixel shall be used with adjusted clock */
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed,
					 adjusted_mode->clock * 1000);
	if (!bytes_pix)
		return;

	dev_dbg(&usb_dev->dev, "Mode requested:  " DRM_MODE_FMT,
		DRM_MODE_ARG(mode));
	dev_dbg(&usb_dev->dev, "Mode configured: " DRM_MODE_FMT,
		DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width =
		adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width =
		adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(fl2000_dev, mode->hdisplay * mode->vdisplay,
			       bytes_pix);
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
				  struct drm_crtc_state *cstate,
				  struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000 *fl2000_dev = drm->dev_private;

	/* TODO: check cstate/pstate? */

	struct drm_display_mode *mode = &cstate->mode;
	if (cstate->mode_changed) {
		fl2000_output_mode_set(fl2000_dev, mode,
				       &cstate->adjusted_mode);
	}

	fl2000_stream_enable(fl2000_dev);

	drm_crtc_vblank_on(crtc);
}

static void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000 *fl2000_dev = drm->dev_private;

	drm_crtc_vblank_off(crtc);

	fl2000_stream_disable(fl2000_dev);
}

static int fl2000_display_check(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *plane_state,
				struct drm_crtc_state *crtc_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_framebuffer *fb = plane_state->fb;
	int n;

	n = fb->format->num_planes;
	if (n > 1) {
		dev_err(drm->dev,
			"Only single plane RGB fbs are supported, got %d planes",
			n);
		return -EINVAL;
	}
	return 0;
}

static void fb2000_dirty(struct drm_framebuffer *fb,
			 const struct iosys_map *map, struct drm_rect *rect)
{
	int ret;
	struct drm_device *drm = fb->dev;
	struct fl2000 *fl2000_dev = drm->dev_private;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return;

	fl2000_stream_compress(fl2000_dev, map->vaddr, fb->height, fb->width,
			       fb->pitches[0]);

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	struct drm_rect rect;
   	struct drm_pending_vblank_event *event = crtc->state->event;
	int idx;

	if (!drm_dev_enter(drm, &idx)) {
		dev_err(drm->dev, "DRM enter failed!");
		return;
	}

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		fb2000_dirty(state->fb, &shadow_plane_state->data[0], &rect);

	drm_dev_exit(idx);

	if (event) {
			crtc->state->event = NULL;

			spin_lock_irq(&drm->event_lock);
			if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
					drm_crtc_arm_vblank_event(crtc, event);
			else
					drm_crtc_send_vblank_event(crtc, event);
			spin_unlock_irq(&drm->event_lock);
	}
}

/* Logical pipe management (no HW configuration here) */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_display_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.check = fl2000_display_check,
	.update = fl2000_display_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS
};

static void fl2000_encoder_mode_set(struct drm_encoder *encoder,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct fl2000 *fl2000_dev = drm->dev_private;
	fl2000_output_mode_set(fl2000_dev, mode, adjusted_mode);
}
/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_encoder_mode_set,
};

/* TODO: release on errors! */
int fl2000_drm_init(struct fl2000 *fl2000_dev)
{
	int ret = 0;
	struct usb_device *usb_dev = fl2000_dev->usb_dev;
	struct usb_interface *if_stream =
		fl2000_dev->intf[FL2000_USBIF_STREAMING];
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	u64 dma_mask;

	drm = &fl2000_dev->drm;
	drm->dev_private = fl2000_dev;

	fl2000_dev->dmadev = usb_intf_get_dma_device(if_stream);
	if (!fl2000_dev->dmadev)
		drm_warn(drm,
			 "buffer sharing not supported"); /* not an error */

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot initialize DRM mode (%d)", ret);
		goto err_put_dmadev;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL2000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL2000_MAX_HEIGHT;
	mode_config->prefer_shadow = 0;
	mode_config->preferred_depth = FL2000_FB_BPP;

	/* Set DMA mask for DRM device from mask of the 'parent' USB device */
	dma_mask = dma_get_mask(&usb_dev->dev);
	ret = dma_set_coherent_mask(drm->dev, dma_mask);
	if (ret) {
		dev_err(drm->dev, "Cannot set DRM device DMA mask (%d)", ret);
		goto err_put_dmadev;
	}

	fl2000_connector_init(fl2000_dev);

	ret = drm_simple_display_pipe_init(drm, &fl2000_dev->pipe,
					   &fl2000_display_funcs,
					   fl2000_pixel_formats,
					   ARRAY_SIZE(fl2000_pixel_formats),
					   NULL, &fl2000_dev->connector);
	if (ret) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)",
			ret);
		goto err_put_dmadev;
	}

	/* Register 'mode_set' function to operate prior to bridge */
	drm_encoder_helper_add(&fl2000_dev->pipe.encoder,
			       &fl2000_encoder_funcs);

	/* Start streaming interface */
	ret = fl2000_stream_create(fl2000_dev);
	if (ret)
		goto err_put_dmadev;

	/* Start interrupts interface */
	ret = fl2000_intr_create(fl2000_dev);
	if (ret)
		goto err_stream_release;

	/* Attach bridge */

	drm_mode_config_reset(drm);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		dev_err(drm->dev, "Failed to initialize %d VBLANK(s) (%d)",
			drm->mode_config.num_crtc, ret);
		goto err_intr_release;
	}

	drm_kms_helper_poll_init(drm);

	drm_plane_enable_fb_damage_clips(&fl2000_dev->pipe.plane);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		goto err_intr_release;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	drm_fbdev_generic_setup(drm, FL2000_FB_BPP);

	return 0;

err_intr_release:
	fl2000_intr_release(fl2000_dev);
err_stream_release:
	fl2000_stream_release(fl2000_dev);
err_put_dmadev:
	put_device(fl2000_dev->dmadev);
	return ret;
}

void fl2000_drm_release(struct fl2000 *fl2000_dev)
{
	struct drm_device *drm = &fl2000_dev->drm;

	drm_crtc_vblank_off(&fl2000_dev->pipe.crtc);

	/* Stop streaming interface */
	fl2000_stream_release(fl2000_dev);

	/* Stop interrupts interface */
	fl2000_intr_release(fl2000_dev);

	/* Prepare to DRM device shutdown */
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	put_device(fl2000_dev->dmadev);
	fl2000_dev->dmadev = NULL;
}
