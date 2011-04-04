/*
 * Copyright © 2010 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "drmP.h"
#include "drm.h"
#include "i915_drm.h"
#include "i915_drv.h"
#include "i915_trace.h"
#include "intel_drv.h"
#include <linux/swap.h>

static struct i915_gem_vmap_object *to_vmap_object(struct drm_i915_gem_object *obj)
{
	return container_of(obj, struct i915_gem_vmap_object, gem);
}

static int
i915_gem_vmap_get_pages(struct drm_i915_gem_object *obj,
			struct page **pages,
			u32 *offset)
{
	struct i915_gem_vmap_object *vmap = to_vmap_object(obj);
	int num_pages = vmap->gem.base.size >> PAGE_SHIFT, n;
	int pinned, ret;

	if (!access_ok(vmap->read_only ? VERIFY_READ : VERIFY_WRITE,
		       (char __user *)vmap->user_ptr, vmap->user_size))
		return -EFAULT;

	/* If userspace should engineer that these pages are replaced in
	 * the vma between us binding this page into the GTT and completion
	 * of rendering... Their loss. If they change the mapping of their
	 * pages they need to create a new bo to point to the new vma.
	 *
	 * However, that still leaves open the possibility of the vma
	 * being copied upon fork. Which falls under the same userspace
	 * synchronisation issue as a regular bo, except that this time
	 * the process may not be expecting that a particular piece of
	 * memory is tied to the GPU.
	 */

	n = num_pages;
	pinned = __get_user_pages_fast(vmap->user_ptr, num_pages,
				       !vmap->read_only, pages);
	if (pinned < num_pages) {
		struct mm_struct *mm = current->mm;

		mutex_unlock(&obj->base.dev->struct_mutex);
		down_read(&mm->mmap_sem);
		ret = get_user_pages(current, mm,
				     vmap->user_ptr + (pinned << PAGE_SHIFT),
				     num_pages - pinned,
				     !vmap->read_only, 0,
				     pages + pinned,
				     NULL);
		up_read(&mm->mmap_sem);
		mutex_lock(&obj->base.dev->struct_mutex);
		if (ret > 0)
			pinned += ret;

		if (pinned < num_pages) {
			release_pages(pages, pinned, 0);
			return -EFAULT;
		}
	}

	obj->dirty = 0;
	*offset = offset_in_page(vmap->user_ptr);
	return 0;
}

static int
i915_gem_vmap_put_pages(struct drm_i915_gem_object *obj)
{
	int num_pages = obj->base.size >> PAGE_SHIFT;
	int i;

	for (i = 0; i < num_pages; i++) {
		if (obj->dirty)
			set_page_dirty(obj->pages[i]);

		mark_page_accessed(obj->pages[i]);
	}
	release_pages(obj->pages, num_pages, 0);

	obj->dirty = 0;
	return 0;
}

static const struct drm_i915_gem_object_ops i915_gem_vmap_ops = {
	.get_pages = i915_gem_vmap_get_pages,
	.put_pages = i915_gem_vmap_put_pages,
};

/**
 * Creates a new mm object that wraps some user memory.
 */
int
i915_gem_vmap_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_vmap *args = data;
	struct i915_gem_vmap_object *obj;
	loff_t first_data_page, last_data_page;
	int num_pages;
	int ret;
	u32 handle;

	first_data_page = args->user_ptr / PAGE_SIZE;
	last_data_page = (args->user_ptr + args->user_size - 1) / PAGE_SIZE;
	num_pages = last_data_page - first_data_page + 1;
	if (num_pages * PAGE_SIZE > dev_priv->mm.gtt_total)
		return -E2BIG;

	ret = fault_in_multipages_readable((char __user *)(uintptr_t)args->user_ptr,
					   args->user_size);
	if (ret)
		return ret;

	/* Allocate the new object */
	obj = i915_gem_object_alloc(dev);
	if (obj == NULL)
		return -ENOMEM;

	if (drm_gem_private_object_init(dev, &obj->gem.base,
					num_pages * PAGE_SIZE)) {
		i915_gem_object_free(&obj->gem);
		return -ENOMEM;
	}

	i915_gem_object_init(&obj->gem, &i915_gem_vmap_ops);
	obj->gem.cache_level = I915_CACHE_LLC_MLC;

	obj->user_ptr = args->user_ptr;
	obj->user_size = args->user_size;
	obj->read_only = args->flags & I915_VMAP_READ_ONLY;

	ret = drm_gem_handle_create(file, &obj->gem.base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->gem.base);
		dev_priv->mm.object_count--;
		dev_priv->mm.object_memory -= obj->gem.base.size;
		i915_gem_object_free(&obj->gem);
		return ret;
	}

	/* drop reference from allocate - handle holds it now */
	drm_gem_object_unreference(&obj->gem.base);

	args->handle = handle;
	return 0;
}
