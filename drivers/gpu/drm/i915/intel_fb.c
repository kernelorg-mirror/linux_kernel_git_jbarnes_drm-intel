/*
 * Copyright © 2007 David Airlie
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     David Airlie
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/sysrq.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/vga_switcheroo.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_helper.h>
#include "intel_drv.h"
#include <drm/i915_drm.h>
#include "i915_drv.h"

static struct fb_ops intelfb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = drm_fb_helper_check_var,
	.fb_set_par = drm_fb_helper_set_par,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_pan_display = drm_fb_helper_pan_display,
	.fb_blank = drm_fb_helper_blank,
	.fb_setcmap = drm_fb_helper_setcmap,
	.fb_debug_enter = drm_fb_helper_debug_enter,
	.fb_debug_leave = drm_fb_helper_debug_leave,
};

static struct fb_info *intelfb_create_info(struct intel_fbdev *ifbdev)
{
	struct drm_framebuffer *fb = &ifbdev->ifb.base;
	struct drm_device *dev = fb->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct fb_info *info;
	u32 gtt_offset, size;
	int ret;

	info = framebuffer_alloc(0, &dev->pdev->dev);
	if (!info)
		return NULL;

	info->par = ifbdev;
	ifbdev->helper.fb = fb;
	ifbdev->helper.fbdev = info;

	strcpy(info->fix.id, "inteldrmfb");

	info->flags = FBINFO_DEFAULT | FBINFO_CAN_FORCE_OUTPUT;
	info->fbops = &intelfb_ops;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto err_info;

	/* setup aperture base/size for vesafb takeover */
	info->apertures = alloc_apertures(1);
	if (!info->apertures)
		goto err_cmap;

	info->apertures->ranges[0].base = dev->mode_config.fb_base;
	info->apertures->ranges[0].size = dev_priv->gtt.mappable_end;

	gtt_offset = ifbdev->ifb.obj->gtt_offset;
	size = ifbdev->ifb.obj->base.size;

	info->fix.smem_start = dev->mode_config.fb_base + gtt_offset;
	info->fix.smem_len = size;

	info->screen_size = size;
	info->screen_base = ioremap_wc(dev_priv->gtt.mappable_base + gtt_offset,
				       size);
	if (!info->screen_base)
		goto err_cmap;

	/* If the object is shmemfs backed, it will have given us zeroed pages.
	 * If the object is stolen however, it will be full of whatever
	 * garbage was left in there.
	 */
	if (ifbdev->ifb.obj->stolen)
		memset_io(info->screen_base, 0, info->screen_size);

	/* Use default scratch pixmap (info->pixmap.flags = FB_PIXMAP_SYSTEM) */

	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->depth);
	drm_fb_helper_fill_var(info, &ifbdev->helper, fb->width, fb->height);

	return info;

err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_info:
	framebuffer_release(info);
	return NULL;
}

static int intelfb_create(struct drm_fb_helper *helper,
			  struct drm_fb_helper_surface_size *sizes)
{
	struct intel_fbdev *ifbdev = (struct intel_fbdev *)helper;
	struct drm_device *dev = ifbdev->helper.dev;
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	struct drm_i915_gem_object *obj;
	struct fb_info *info;
	int size, ret;

	/* we don't do packed 24bpp */
	if (sizes->surface_bpp == 24)
		sizes->surface_bpp = 32;

	mode_cmd.width  = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;

	mode_cmd.pitches[0] =
		intel_framebuffer_pitch_for_width(mode_cmd.width,
						  sizes->surface_bpp);
	mode_cmd.pixel_format =
		drm_mode_legacy_fb_format(sizes->surface_bpp,
					  sizes->surface_depth);

	size = mode_cmd.pitches[0] * mode_cmd.height;
	size = ALIGN(size, PAGE_SIZE);
	obj = i915_gem_object_create_stolen(dev, size);
	if (obj == NULL)
		obj = i915_gem_alloc_object(dev, size);
	if (!obj) {
		DRM_ERROR("failed to allocate framebuffer\n");
		ret = -ENOMEM;
		goto out;
	}

	mutex_lock(&dev->struct_mutex);

	/* Flush everything out, we'll be doing GTT only from now on */
	ret = intel_pin_and_fence_fb_obj(dev, obj, NULL);
	if (ret) {
		DRM_ERROR("failed to pin fb: %d\n", ret);
		goto out_unref;
	}

	ret = intel_framebuffer_init(dev, &ifbdev->ifb, &mode_cmd, obj);
	if (ret)
		goto out_unpin;

	DRM_DEBUG_KMS("allocated %dx%d fb: 0x%08x, bo %p\n",
		      mode_cmd.width, mode_cmd.height,
		      obj->gtt_offset, obj);

	info = intelfb_create_info(ifbdev);
	if (info == NULL) {
		ret = -ENOMEM;
		goto out_unpin;
	}

	mutex_unlock(&dev->struct_mutex);
	return 0;

out_unpin:
	i915_gem_object_unpin(obj);
out_unref:
	drm_gem_object_unreference(&obj->base);
	mutex_unlock(&dev->struct_mutex);
out:
	return ret;
}

static struct drm_fb_helper_crtc *
intel_fb_helper_crtc(struct drm_fb_helper *fb_helper, struct drm_crtc *crtc)
{
	int i;

	for (i = 0; i < fb_helper->crtc_count; i++)
		if (fb_helper->crtc_info[i].mode_set.crtc == crtc)
			return &fb_helper->crtc_info[i];

	return NULL;
}

static bool intel_fb_initial_config(struct drm_fb_helper *fb_helper,
				    struct drm_fb_helper_crtc **crtcs,
				    struct drm_display_mode **modes,
				    bool *enabled, int width, int height)
{
	int i;

	for (i = 0; i < fb_helper->connector_count; i++) {
		struct drm_connector *connector;
		struct drm_encoder *encoder;

		connector = fb_helper->connector_info[i]->connector;
		if (!enabled[i]) {
			DRM_DEBUG_KMS("connector %d not enabled, skipping\n",
				      connector->base.id);
			continue;
		}

		encoder = connector->encoder;
		if (!encoder || !encoder->crtc) {
			DRM_DEBUG_KMS("connector %d has no encoder or crtc, skipping\n",
				      connector->base.id);
			continue;
		}

		if (WARN_ON(!encoder->crtc->enabled)) {
			DRM_DEBUG_KMS("connector %s on crtc %d has inconsistent state, aborting\n",
				      drm_get_connector_name(connector),
				      encoder->crtc->base.id);
			return false;
		}

		if (!to_intel_crtc(encoder->crtc)->mode_valid) {
			DRM_DEBUG_KMS("connector %s on crtc %d has an invalid mode, aborting\n",
				      drm_get_connector_name(connector),
				      encoder->crtc->base.id);
			return false;
		}

		modes[i] = &encoder->crtc->mode;
		crtcs[i] = intel_fb_helper_crtc(fb_helper, encoder->crtc);

		DRM_DEBUG_KMS("connector %s on crtc %d: %s\n",
			      drm_get_connector_name(connector),
			      encoder->crtc->base.id,
			      modes[i]->name);
	}

	return true;
}

static struct drm_fb_helper_funcs intel_fb_helper_funcs = {
	.gamma_set = intel_crtc_fb_gamma_set,
	.gamma_get = intel_crtc_fb_gamma_get,
	.fb_probe = intelfb_create,
};

static void intel_fbdev_destroy(struct drm_device *dev,
				struct intel_fbdev *ifbdev)
{
	struct intel_framebuffer *ifb = &ifbdev->ifb;

	if (ifbdev->helper.fbdev) {
		struct fb_info *info = ifbdev->helper.fbdev;

		unregister_framebuffer(info);
		iounmap(info->screen_base);
		fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}

	drm_fb_helper_fini(&ifbdev->helper);

	drm_framebuffer_unregister_private(&ifb->base);
	drm_framebuffer_cleanup(&ifb->base);
	if (ifb->obj) {
		drm_gem_object_unreference_unlocked(&ifb->obj->base);
		ifb->obj = NULL;
	}
}

static bool pipe_enabled(struct drm_i915_private *dev_priv, enum pipe pipe)
{
	enum transcoder cpu_transcoder =
		intel_pipe_to_cpu_transcoder(dev_priv, pipe);
	return !!(I915_READ(PIPECONF(cpu_transcoder)) & PIPECONF_ENABLE);
}

/*
 * Try to read the BIOS display configuration and use it for the initial
 * fb configuration.
 *
 * The BIOS or boot loader will generally create an initial display
 * configuration for us that includes some set of active pipes and displays.
 * This routine tries to figure out which pipes are active, what resolutions
 * are being displayed, and then allocates a framebuffer and initial fb
 * config based on that data.
 *
 * If the BIOS or boot loader leaves the display in VGA mode, there's not
 * much we can do; switching out of that mode involves allocating a new,
 * high res buffer, and also recalculating bandwidth requirements for the
 * new bpp configuration.
 *
 * However, if we're loaded into an existing, high res mode, we should
 * be able to allocate a buffer big enough to handle the largest active
 * mode, create a mode_set for it, and pass it to the fb helper to create
 * the configuration.
 */
void intel_fbdev_init_bios(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_fbdev *ifbdev;
	struct drm_crtc *crtc;
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	struct drm_i915_gem_object *obj;
	u32 obj_offset = 0;
	int mode_bpp = 0;
	u32 active = 0;

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
		int pipe = intel_crtc->pipe, plane = intel_crtc->plane;
		u32 val, bpp, offset, format;
		int pitch, width, height;

		if (!pipe_enabled(dev_priv, pipe)) {
			DRM_DEBUG_KMS("pipe %c not active, skipping\n",
				      pipe_name(pipe));
			continue;
		}

		val = I915_READ(DSPCNTR(plane));

		if (INTEL_INFO(dev)->gen >= 4) {
			if (val & DISPPLANE_TILED) {
				DRM_DEBUG_KMS("tiled BIOS fb?\n");
				continue; /* unexpected! */
			}
		}

		switch (val & DISPPLANE_PIXFORMAT_MASK) {
		case DISPPLANE_YUV422:
		default:
			DRM_DEBUG_KMS("pipe %c unsupported pixel format %x, skipping\n",
				      pipe_name(pipe), (val & DISPPLANE_PIXFORMAT_MASK) >> 26);
			continue;
		case DISPPLANE_8BPP:
			format = DRM_FORMAT_C8;
			bpp = 8;
			break;
		case DISPPLANE_BGRX555:
			format = DRM_FORMAT_XRGB1555;
			bpp = 16;
			break;
		case DISPPLANE_BGRX565:
			format = DRM_FORMAT_RGB565;
			bpp = 16;
			break;
		case DISPPLANE_BGRX888:
			format = DRM_FORMAT_XRGB8888;
			bpp = 32;
			break;
		}

		if (mode_cmd.pixel_format == 0) {
			mode_bpp = bpp;
			mode_cmd.pixel_format = format;
		}

		if (mode_cmd.pixel_format != format) {
			DRM_DEBUG_KMS("pipe %c has format/bpp (%d, %d) mismatch: skipping\n",
				      pipe_name(pipe), format, bpp);
			continue;
		}

		if (INTEL_INFO(dev)->gen >= 4) {
			if (I915_READ(DSPTILEOFF(plane))) {
				DRM_DEBUG_KMS("pipe %c is offset: skipping\n",
					      pipe_name(pipe));
				continue;
			}

			offset = I915_READ(DSPSURF(plane));
		} else {
			offset = I915_READ(DSPADDR(plane));
		}
		if (!obj_offset)
			obj_offset = offset;

		if (offset != obj_offset) {
			DRM_DEBUG_KMS("multiple pipe setup not in clone mode, skipping\n");
			continue;
		}

		val = I915_READ(PIPESRC(pipe));
		width = ((val >> 16) & 0xfff) + 1;
		height = ((val >> 0) & 0xfff) + 1;

		/* Adjust fitted modes */
		val = I915_READ(HTOTAL(pipe));
		if (((val & 0xffff) + 1) != width) {
			DRM_DEBUG_DRIVER("BIOS fb not native width (%d vs %d), overriding\n", width, (val & 0xffff) + 1);
			width = (val & 0xffff) + 1;
		}
		val = I915_READ(VTOTAL(pipe));
		if (((val & 0xffff) + 1) != height) {
			DRM_DEBUG_DRIVER("BIOS fb not native height (%d vs %d), overriding\n", height, (val & 0xffff) + 1);
			height = (val & 0xffff) + 1;
		}

		DRM_DEBUG_KMS("Found active pipe [%d/%d]: size=%dx%d@%d, offset=%x\n",
			      pipe, plane, width, height, bpp, offset);

		if (width > mode_cmd.width)
			mode_cmd.width = width;

		if (height > mode_cmd.height)
			mode_cmd.height = height;

		pitch = intel_framebuffer_pitch_for_width(width, bpp);
		if (pitch > mode_cmd.pitches[0])
			mode_cmd.pitches[0] = pitch;

		active |= 1 << pipe;
	}

	if (active == 0) {
		DRM_DEBUG_KMS("no active pipes found, not using BIOS config\n");
		return;
	}

	ifbdev = kzalloc(sizeof(struct intel_fbdev), GFP_KERNEL);
	if (ifbdev == NULL) {
		DRM_DEBUG_KMS("failed to alloc intel fbdev\n");
		return;
	}

	ifbdev->stolen = true;
	ifbdev->preferred_bpp = mode_bpp;
	ifbdev->helper.funcs = &intel_fb_helper_funcs;
	ifbdev->helper.funcs->initial_config = intel_fb_initial_config;

	/* assume a 1:1 linear mapping between stolen and GTT */
	obj = i915_gem_object_create_stolen_for_preallocated(dev,
							     obj_offset,
							     obj_offset,
							     ALIGN(mode_cmd.pitches[0] * mode_cmd.height, PAGE_SIZE));
	if (obj == NULL) {
		DRM_DEBUG_KMS("failed to create stolen fb\n");
		goto out_free_ifbdev;
	}

	if (intel_framebuffer_init(dev, &ifbdev->ifb, &mode_cmd, obj)) {
		DRM_DEBUG_KMS("intel fb init failed\n");
		goto out_unref_obj;
	}

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		if ((active & (1 << to_intel_crtc(crtc)->pipe)) == 0)
			continue;

		/*
		 * This should only fail on the first one so we don't need
		 * to cleanup any secondary crtc->fbs
		 */
		if (intel_pin_and_fence_fb_obj(dev, obj, NULL))
			goto out_unref_obj;

		crtc->fb = &ifbdev->ifb.base;
	}

	dev_priv->fbdev = ifbdev;

	DRM_DEBUG_KMS("using BIOS fb for initial console\n");
	return;

out_unref_obj:
	drm_gem_object_unreference_unlocked(&obj->base);
out_free_ifbdev:
	kfree(ifbdev);
}

int intel_fbdev_init(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_fbdev *ifbdev;
	int ret;

	if ((ifbdev = dev_priv->fbdev) == NULL) {
		ifbdev = kzalloc(sizeof(struct intel_fbdev), GFP_KERNEL);
		if (ifbdev == NULL)
			return -ENOMEM;

		ifbdev->helper.funcs = &intel_fb_helper_funcs;
		ifbdev->preferred_bpp = 32;

		dev_priv->fbdev = ifbdev;
	}

	ret = drm_fb_helper_init(dev, &ifbdev->helper,
				 INTEL_INFO(dev)->num_pipes,
				 INTELFB_CONN_LIMIT);
	if (ret) {
		dev_priv->fbdev = NULL;
		kfree(ifbdev);
		return ret;
	}

	drm_fb_helper_single_add_all_connectors(&ifbdev->helper);

	return 0;
}

void intel_fbdev_initial_config(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_fbdev *ifbdev = dev_priv->fbdev;

	/* Due to peculiar init order wrt to hpd handling this is separate. */
	drm_fb_helper_initial_config(&ifbdev->helper, ifbdev->preferred_bpp);
}

void intel_fbdev_fini(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	if (!dev_priv->fbdev)
		return;

	intel_fbdev_destroy(dev, dev_priv->fbdev);
	kfree(dev_priv->fbdev);
	dev_priv->fbdev = NULL;
}

void intel_fbdev_set_suspend(struct drm_device *dev, int state)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	if (!dev_priv->fbdev)
		return;

	fb_set_suspend(dev_priv->fbdev->helper.fbdev, state);
}

MODULE_LICENSE("GPL and additional rights");

void intel_fb_output_poll_changed(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	if (dev_priv->fbdev)
		drm_fb_helper_hotplug_event(&dev_priv->fbdev->helper);
}

void intel_fb_restore_mode(struct drm_device *dev)
{
	int ret;
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_plane *plane;

	drm_modeset_lock_all(dev);

	ret = drm_fb_helper_restore_fbdev_mode(&dev_priv->fbdev->helper);
	if (ret)
		DRM_DEBUG("failed to restore crtc mode\n");

	/* Be sure to shut off any planes that may be active */
	list_for_each_entry(plane, &config->plane_list, head)
		if (plane->enabled)
			plane->funcs->disable_plane(plane);

	drm_modeset_unlock_all(dev);
}
