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

#include "drmP.h"
#include "drm.h"
#include "drm_crtc.h"
#include "drm_fb_helper.h"
#include "intel_drv.h"
#include "i915_drm.h"
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
	info->apertures->ranges[0].size =
		dev_priv->mm.gtt->gtt_mappable_entries << PAGE_SHIFT;

	gtt_offset = ifbdev->ifb.obj->gtt_offset;
	size = ifbdev->ifb.obj->base.size;

	info->fix.smem_start = dev->mode_config.fb_base + gtt_offset;
	info->fix.smem_len = size;

	info->screen_size = size;
	info->screen_base = ioremap_wc(dev->agp->base + gtt_offset, size);
	if (!info->screen_base)
		goto err_cmap;

	/* Use default scratch pixmap (info->pixmap.flags = FB_PIXMAP_SYSTEM) */

	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->depth);
	drm_fb_helper_fill_var(info, &ifbdev->helper, fb->width, fb->height);

	return info;

err_cmap:
	if (info->cmap.len)
		fb_dealloc_cmap(&info->cmap);
err_info:
	framebuffer_release(info);
	return NULL;
}

static int intelfb_create(struct intel_fbdev *ifbdev,
			  struct drm_fb_helper_surface_size *sizes)
{
	struct drm_device *dev = ifbdev->helper.dev;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_i915_gem_object *obj;
	struct fb_info *info;
	int size, ret;

	/* we don't do packed 24bpp */
	if (sizes->surface_bpp == 24)
		sizes->surface_bpp = 32;

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;

	mode_cmd.pitches[0] = ALIGN(mode_cmd.width * ((sizes->surface_bpp + 7) /
						      8), 64);
	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
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
	vga_switcheroo_client_fb_set(dev->pdev, info);
	return 0;

out_unpin:
	i915_gem_object_unpin(obj);
out_unref:
	drm_gem_object_unreference(&obj->base);
	mutex_unlock(&dev->struct_mutex);
out:
	return ret;
}

static int intel_fb_find_or_create_single(struct drm_fb_helper *helper,
					  struct drm_fb_helper_surface_size *sizes)
{
	struct intel_fbdev *ifbdev = (struct intel_fbdev *)helper;
	int new_fb = 0;
	int ret;

	/* A stolen BIOS fb may be new to the core */
	if (ifbdev->bios_fb && !ifbdev->bios_fb_registered) {
		ifbdev->bios_fb_registered = true;
		return 1;
	}

	if (!helper->fb) {
		ret = intelfb_create(ifbdev, sizes);
		if (ret)
			return ret;
		new_fb = 1;
	}
	return new_fb;
}

static struct drm_fb_helper_crtc *intel_fb_helper_crtc(struct drm_fb_helper *fb_helper, struct drm_crtc *crtc)
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
	struct intel_fbdev *ifbdev = (struct intel_fbdev *)fb_helper;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	int i;

	if (!ifbdev->bios_fb)
		return false;

	for (i = 0; i < fb_helper->connector_count; i++) {
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

		modes[i] = &encoder->crtc->mode;
		crtcs[i] = intel_fb_helper_crtc(fb_helper, encoder->crtc);
		DRM_DEBUG_KMS("fb crtc info: %p\n", crtcs[i]);
		DRM_DEBUG_KMS("connector %s on crtc %d: %s\n",
			      drm_get_connector_name(connector),
			      encoder->crtc->base.id,
			      modes[i] ? modes[i]->name : "off");
	}

	return true;
}

static struct drm_fb_helper_funcs intel_fb_helper_funcs = {
	.gamma_set = intel_crtc_fb_gamma_set,
	.gamma_get = intel_crtc_fb_gamma_get,
	.fb_probe = intel_fb_find_or_create_single,
	.initial_config = intel_fb_initial_config,
};

static void intel_fbdev_destroy(struct drm_device *dev,
				struct intel_fbdev *ifbdev)
{
	struct intel_framebuffer *ifb = &ifbdev->ifb;

	if (ifbdev->helper.fbdev) {
		struct fb_info *info = ifbdev->helper.fbdev;

		unregister_framebuffer(info);
		iounmap(info->screen_base);
		if (info->cmap.len)
			fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}

	drm_fb_helper_fini(&ifbdev->helper);

	drm_framebuffer_cleanup(&ifb->base);
	if (ifb->obj) {
		drm_gem_object_unreference_unlocked(&ifb->obj->base);
		ifb->obj = NULL;
	}
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
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_i915_gem_object *obj;
	struct fb_info *info;
	u32 obj_size = 0, obj_offset = 0, last_bpp = 0, last_depth = 0;
	int ret;

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
		int pipe = intel_crtc->pipe, plane = intel_crtc->plane;
		u32 val, bpp, depth, offset;
		int pitch, width, height, size;

		if (!intel_crtc->active) {
			DRM_DEBUG_KMS("pipe %d not active, skipping\n", pipe);
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
		default:
		case DISPPLANE_8BPP:
			depth = bpp = 8;
			break;
		case DISPPLANE_15_16BPP:
			depth = 15; bpp = 16;
			break;
		case DISPPLANE_16BPP:
			depth = bpp = 16;
			break;
		case DISPPLANE_32BPP_NO_ALPHA:
			depth = 24; bpp = 32;
			break;
		}

		if (!last_bpp)
			last_bpp = bpp;
		if (!last_depth)
			last_depth = depth;

		if (bpp != last_bpp || depth != last_depth) {
			DRM_DEBUG_KMS("pipe %d has depth/bpp mismatch: "
				      "(%d/%d vs %d/%d), skipping\n",
				      pipe, bpp, depth, last_bpp, last_depth);
			continue;
		}

		last_bpp = bpp;
		last_depth = depth;

		if (INTEL_INFO(dev)->gen >= 4)
			offset = I915_READ(DSPSURF(plane));
		else
			offset = I915_READ(DSPADDR(plane));

		pitch = I915_READ(DSPSTRIDE(plane));

		val = I915_READ(PIPESRC(pipe));
		width = ((val >> 16) & 0xfff) + 1;
		height = ((val >> 0) & 0xfff) + 1;

		DRM_DEBUG_KMS("Found active pipe [%d/%d]: size=%dx%d@%d, offset=%x\n",
			      pipe, plane, width, height, depth, offset);

		size = pitch * height;
		size = ALIGN(size, PAGE_SIZE);

		/* In case we're handed multiple fbs, inherit the largest one */
		if (!obj_size || size > obj_size) {
			obj_size = size;
			obj_offset = offset;

			mode_cmd.pitches[0] = pitch;
			mode_cmd.width = width;
			mode_cmd.height = height;
			mode_cmd.pixel_format =
				drm_mode_legacy_fb_format(bpp, depth);
		}
	}

	if (!obj_size) {
		DRM_DEBUG_KMS("no active pipes found, not using BIOS config\n");
		goto out_fail;
	}

	ifbdev = kzalloc(sizeof(struct intel_fbdev), GFP_KERNEL);
	if (!ifbdev) {
		DRM_DEBUG_KMS("failed to alloc intel fbdev\n");
		goto out_fail;
	}

	ifbdev->helper.funcs = &intel_fb_helper_funcs;
	ret = drm_fb_helper_init(dev, &ifbdev->helper,
				 dev_priv->num_pipe,
				 INTELFB_CONN_LIMIT);
	if (ret) {
		DRM_DEBUG_KMS("drm fb init failed\n");
		goto out_free_ifbdev;
	}

	/* assume a 1:1 linear mapping between stolen and GTT */
	obj = i915_gem_object_create_stolen_for_preallocated(dev, obj_offset,
							     obj_offset,
							     obj_size);
	if (obj == NULL) {
		DRM_DEBUG_KMS("failed to create stolen fb\n");
		goto out_free_ifbdev;
	}

	ret = intel_framebuffer_init(dev, &ifbdev->ifb, &mode_cmd, obj);
	if (ret) {
		DRM_DEBUG_KMS("intel fb init failed\n");
		goto out_unref_obj;
	}

	info = intelfb_create_info(ifbdev);
	if (info == NULL) {
		DRM_DEBUG_KMS("intelfb fb creation failed\n");
		goto out_unref_obj;
	}

	/* FIXME: omit any crtcs left out in the above loop */
	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
		if (!intel_crtc->active)
			continue;

		crtc->fb = &ifbdev->ifb.base;
		obj->pin_count++;
	}
	ifbdev->bios_fb = true;

	drm_fb_helper_single_add_all_connectors(&ifbdev->helper);
	drm_fb_helper_initial_config(&ifbdev->helper, last_bpp);

	vga_switcheroo_client_fb_set(dev->pdev, info);
	dev_priv->fbdev = ifbdev;

	DRM_DEBUG_KMS("using BIOS config for initial console\n");

	return;

out_unref_obj:
	drm_gem_object_unreference(&obj->base);
out_free_ifbdev:
	kfree(ifbdev);
out_fail:
	/* otherwise disable all the possible crtcs before KMS */
	drm_helper_disable_unused_functions(dev);
}

int intel_fbdev_init(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_fbdev *ifbdev;
	int ret;

	if (dev_priv->fbdev)
		return 0;

	ifbdev = kzalloc(sizeof(struct intel_fbdev), GFP_KERNEL);
	if (ifbdev == NULL)
		return -ENOMEM;

	ifbdev->helper.funcs = &intel_fb_helper_funcs;
	ret = drm_fb_helper_init(dev, &ifbdev->helper,
				 dev_priv->num_pipe,
				 INTELFB_CONN_LIMIT);
	if (ret) {
		kfree(ifbdev);
		return ret;
	}

	dev_priv->fbdev = ifbdev;

	drm_fb_helper_single_add_all_connectors(&ifbdev->helper);
	drm_fb_helper_initial_config(&ifbdev->helper, 32);
	return 0;
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
	drm_fb_helper_hotplug_event(&dev_priv->fbdev->helper);
}

void intel_fb_restore_mode(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_fb_helper *helper = &dev_priv->fbdev->helper;
	struct drm_plane *plane;
	int ret;

	mutex_lock(&dev->mode_config.mutex);

	ret = drm_fb_helper_restore_fbdev_mode(helper);
	if (ret)
		DRM_DEBUG("failed to restore crtc mode\n");

	if (helper->delayed_hotplug) {
		helper->delayed_hotplug = false;
		drm_fb_helper_hotplug_event(helper);
	}

	/* Be sure to shut off any planes that may be active */
	list_for_each_entry(plane, &config->plane_list, head)
		plane->funcs->disable_plane(plane);

	mutex_unlock(&dev->mode_config.mutex);
}
