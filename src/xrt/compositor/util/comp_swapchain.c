// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Independent swapchain implementation.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_util
 */

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_os.h"

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"

#include "util/comp_swapchain.h"
#include "vk/vk_cmd_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>


/*
 *
 * Swapchain member functions.
 *
 */

static void
swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	VK_TRACE(sc->vk, "DESTROY");

	u_threading_stack_push(&sc->cscs->destroy_swapchains, sc);
}

static xrt_result_t
swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	VK_TRACE(sc->vk, "ACQUIRE_IMAGE");

	// Returns negative on empty fifo.
	int res = u_index_fifo_pop(&sc->fifo, out_index);
	if (res >= 0) {
		return XRT_SUCCESS;
	}
	return XRT_ERROR_NO_IMAGE_AVAILABLE;
}

static xrt_result_t
swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	SWAPCHAIN_TRACE_BEGIN(swapchain_inc_image_use);

	VK_TRACE(sc->vk, "%p INC_IMAGE %d (use %d)", (void *)sc, index, sc->images[index].use_count);

	os_mutex_lock(&sc->images[index].use_mutex);
	sc->images[index].use_count++;
	os_mutex_unlock(&sc->images[index].use_mutex);

	SWAPCHAIN_TRACE_END(swapchain_inc_image_use);

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	SWAPCHAIN_TRACE_BEGIN(swapchain_dec_image_use);

	VK_TRACE(sc->vk, "%p DEC_IMAGE %d (use %d)", (void *)sc, index, sc->images[index].use_count);

	os_mutex_lock(&sc->images[index].use_mutex);

	assert(sc->images[index].use_count > 0 && "use count already 0");

	sc->images[index].use_count--;
	if (sc->images[index].use_count == 0) {
		os_mutex_unlock(&sc->images[index].use_mutex);
		pthread_cond_broadcast(&sc->images[index].use_cond);
	}

	os_mutex_unlock(&sc->images[index].use_mutex);

	SWAPCHAIN_TRACE_END(swapchain_dec_image_use);

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_wait_image(struct xrt_swapchain *xsc, uint64_t timeout_ns, uint32_t index)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	SWAPCHAIN_TRACE_BEGIN(swapchain_wait_image);

	VK_TRACE(sc->vk, "%p WAIT_IMAGE %d (use %d)", (void *)sc, index, sc->images[index].use_count);

	os_mutex_lock(&sc->images[index].use_mutex);

	if (sc->images[index].use_count == 0) {
		VK_TRACE(sc->vk, "%p WAIT_IMAGE %d: NO WAIT", (void *)sc, index);
		os_mutex_unlock(&sc->images[index].use_mutex);
		SWAPCHAIN_TRACE_END(swapchain_wait_image);
		return XRT_SUCCESS;
	}

	// on windows pthread_cond_timedwait can not be used with monotonic time
	uint64_t start_wait_rt = os_realtime_get_ns();

	uint64_t end_wait_rt;
	// don't wrap on big or indefinite timeout
	if (start_wait_rt > UINT64_MAX - timeout_ns) {
		end_wait_rt = UINT64_MAX;
	} else {
		end_wait_rt = start_wait_rt + timeout_ns;
	}

	struct timespec spec;
	os_ns_to_timespec(end_wait_rt, &spec);

	VK_TRACE(sc->vk, "%p WAIT_IMAGE %d (use %d) start wait at: %" PRIu64 " (timeout at %" PRIu64 ")", (void *)sc,
	         index, sc->images[index].use_count, start_wait_rt, end_wait_rt);

	int ret = 0;
	while (sc->images[index].use_count > 0) {
		// use pthread_cond_timedwait to implement timeout behavior
		ret = pthread_cond_timedwait(&sc->images[index].use_cond, &sc->images[index].use_mutex.mutex, &spec);

		uint64_t now_rt = os_realtime_get_ns();
		double diff = time_ns_to_ms_f(now_rt - start_wait_rt);

		if (ret == 0) {

			if (sc->images[index].use_count == 0) {
				// image became available within timeout limits
				VK_TRACE(sc->vk, "%p WAIT_IMAGE %d: success at %" PRIu64 " after %fms", (void *)sc,
				         index, now_rt, diff);
				os_mutex_unlock(&sc->images[index].use_mutex);
				SWAPCHAIN_TRACE_END(swapchain_wait_image);
				return XRT_SUCCESS;
			}
			// cond got signaled but image is still in use, continue waiting
			VK_TRACE(sc->vk, "%p WAIT_IMAGE %d: woken at %" PRIu64 " after %fms but still (%d use)",
			         (void *)sc, index, now_rt, diff, sc->images[index].use_count);
			continue;
		}

		if (ret == ETIMEDOUT) {
			VK_TRACE(sc->vk, "%p WAIT_IMAGE %d (use %d): timeout at %" PRIu64 " after %fms", (void *)sc,
			         index, sc->images[index].use_count, now_rt, diff);

			if (now_rt >= end_wait_rt) {
				// image did not become available within timeout limits
				VK_TRACE(sc->vk, "%p WAIT_IMAGE %d (use %d): timeout (%" PRIu64 " > %" PRIu64 ")",
				         (void *)sc, index, sc->images[index].use_count, now_rt, end_wait_rt);
				os_mutex_unlock(&sc->images[index].use_mutex);
				SWAPCHAIN_TRACE_END(swapchain_wait_image);
				return XRT_TIMEOUT;
			}
			// spurious cond wakeup
			VK_TRACE(sc->vk, "%p WAIT_IMAGE %d (use %d): spurious timeout at %" PRIu64 " (%fms to timeout)",
			         (void *)sc, index, sc->images[index].use_count, now_rt,
			         time_ns_to_ms_f(end_wait_rt - now_rt));
			continue;
		}

		// if no other case applied
		VK_TRACE(sc->vk, "%p WAIT_IMAGE %d: condition variable error %d", (void *)sc, index, ret);
		os_mutex_unlock(&sc->images[index].use_mutex);
		SWAPCHAIN_TRACE_END(swapchain_wait_image);
		return XRT_ERROR_VULKAN;
	}

	VK_TRACE(sc->vk, "%p WAIT_IMAGE %d: became available before spurious wakeup %d", (void *)sc, index, ret);

	os_mutex_unlock(&sc->images[index].use_mutex);
	SWAPCHAIN_TRACE_END(swapchain_wait_image);

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_swapchain *sc = comp_swapchain(xsc);

	VK_TRACE(sc->vk, "RELEASE_IMAGE");

	int res = u_index_fifo_push(&sc->fifo, index);

	if (res >= 0) {
		return XRT_SUCCESS;
	}
	// FIFO full
	return XRT_ERROR_NO_IMAGE_AVAILABLE;
}


/*
 *
 * Helper functions.
 *
 */

#define D(TYPE, thing)                                                                                                 \
	if (thing != VK_NULL_HANDLE) {                                                                                 \
		vk->vkDestroy##TYPE(vk->device, thing, NULL);                                                          \
		thing = VK_NULL_HANDLE;                                                                                \
	}

static struct comp_swapchain *
set_common_fields(struct comp_swapchain *sc,
                  comp_swapchain_destroy_func_t destroy_func,
                  struct vk_bundle *vk,
                  struct comp_swapchain_shared *cscs,
                  uint32_t image_count)
{
	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire_image;
	sc->base.base.inc_image_use = swapchain_inc_image_use;
	sc->base.base.dec_image_use = swapchain_dec_image_use;
	sc->base.base.wait_image = swapchain_wait_image;
	sc->base.base.release_image = swapchain_release_image;
	sc->base.base.image_count = image_count;
	sc->real_destroy = destroy_func;
	sc->vk = vk;
	sc->cscs = cscs;

	// Make sure the handles are invalid.
	for (uint32_t i = 0; i < ARRAY_SIZE(sc->base.images); i++) {
		sc->base.images[i].handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}

	return sc;
}

static void
do_post_create_vulkan_setup(struct vk_bundle *vk,
                            const struct xrt_swapchain_create_info *info,
                            struct comp_swapchain *sc)
{
	uint32_t image_count = sc->vkic.image_count;
	VkCommandBuffer cmd_buffer;
	VkResult ret;

	VkComponentMapping components = {
	    .r = VK_COMPONENT_SWIZZLE_R,
	    .g = VK_COMPONENT_SWIZZLE_G,
	    .b = VK_COMPONENT_SWIZZLE_B,
	    .a = VK_COMPONENT_SWIZZLE_ONE,
	};

	// This is the format for the image view, it's not adjusted.
	VkFormat image_view_format = (VkFormat)info->format;
	VkImageAspectFlagBits image_view_aspect = vk_csci_get_image_view_aspect(image_view_format, info->bits);

	VkImageViewType image_view_type = info->face_count == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;

	for (uint32_t i = 0; i < image_count; i++) {
		sc->images[i].views.alpha = U_TYPED_ARRAY_CALLOC(VkImageView, info->array_size);
		sc->images[i].views.no_alpha = U_TYPED_ARRAY_CALLOC(VkImageView, info->array_size);
		sc->images[i].array_size = info->array_size;

		for (uint32_t layer = 0; layer < info->array_size; ++layer) {
			VkImageSubresourceRange subresource_range = {
			    .aspectMask = image_view_aspect,
			    .baseMipLevel = 0,
			    .levelCount = 1,
			    .baseArrayLayer = layer * info->face_count,
			    .layerCount = info->face_count,
			};

			vk_create_view(                         //
			    vk,                                 // vk
			    sc->vkic.images[i].handle,          // image
			    image_view_type,                    // type
			    image_view_format,                  // format
			    subresource_range,                  // subresource_range
			    &sc->images[i].views.alpha[layer]); // out_view

			vk_create_view_swizzle(                    //
			    vk,                                    // vk
			    sc->vkic.images[i].handle,             // image
			    image_view_type,                       // type
			    image_view_format,                     // format
			    subresource_range,                     // subresource_range
			    components,                            // components
			    &sc->images[i].views.no_alpha[layer]); // out_view
		}
	}

	// Prime the fifo
	for (uint32_t i = 0; i < image_count; i++) {
		u_index_fifo_push(&sc->fifo, i);
	}

	/*
	 *
	 * Transition image.
	 *
	 */

	// To reduce the pointer chasing.
	struct vk_cmd_pool *pool = &sc->cscs->pool;

	// First lock.
	vk_cmd_pool_lock(pool);

	// Now lets create the command buffer.
	ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, pool, 0, &cmd_buffer);
	if (ret != VK_SUCCESS) {
		vk_cmd_pool_unlock(pool);
		VK_ERROR(vk, "Failed to barrier images");
		return;
	}

	VkImageAspectFlagBits image_barrier_aspect = vk_csci_get_barrier_aspect_mask(image_view_format);

	VkImageSubresourceRange subresource_range = {
	    .aspectMask = image_barrier_aspect,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = info->array_size * info->face_count,
	};

	for (uint32_t i = 0; i < image_count; i++) {
		vk_cmd_image_barrier_gpu_locked(              //
		    vk,                                       //
		    cmd_buffer,                               //
		    sc->vkic.images[i].handle,                //
		    0,                                        //
		    VK_ACCESS_SHADER_READ_BIT,                //
		    VK_IMAGE_LAYOUT_UNDEFINED,                //
		    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, //
		    subresource_range);                       //
	}

	// Done writing commands, submit to queue, waits for command to finish.
	ret = vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked(vk, pool, cmd_buffer);

	// Done submitting commands.
	vk_cmd_pool_unlock(pool);

	// Check results from submit.
	if (ret != VK_SUCCESS) {
		//! @todo Propegate error
		VK_ERROR(vk, "Failed to barrier images");
	}

	for (uint32_t i = 0; i < image_count; i++) {

		ret = pthread_cond_init(&sc->images[i].use_cond, NULL);
		if (ret) {
			VK_ERROR(sc->vk, "Failed to init image use cond: %d", ret);
			continue;
		}

		ret = os_mutex_init(&sc->images[i].use_mutex);
		if (ret) {
			VK_ERROR(sc->vk, "Failed to init image use mutex: %d", ret);
			continue;
		}

		sc->images[i].use_count = 0;
	}
}

static void
clean_image_views(struct vk_bundle *vk, size_t array_size, VkImageView **views_ptr)
{
	VkImageView *views = *views_ptr;
	if (views == NULL) {
		return;
	}

	for (uint32_t i = 0; i < array_size; ++i) {
		if (views[i] == VK_NULL_HANDLE) {
			continue;
		}

		D(ImageView, views[i]);
	}

	free(views);
	array_size = 0;

	*views_ptr = NULL;
}

/*!
 * Free and destroy any initialized fields on the given image, safe to pass in
 * images that has one or all fields set to NULL.
 */
static void
image_cleanup(struct vk_bundle *vk, struct comp_swapchain_image *image)
{
	/*
	 * This makes sure that any pending command buffer has completed and all
	 * resources referred by it can now be manipulated. This make sure that
	 * validation doesn't complain. This is done during image destruction so
	 * isn't time critical.
	 */
	os_mutex_lock(&vk->queue_mutex);
	vk->vkDeviceWaitIdle(vk->device);
	os_mutex_unlock(&vk->queue_mutex);

	clean_image_views(vk, image->array_size, &image->views.alpha);
	clean_image_views(vk, image->array_size, &image->views.no_alpha);
}

/*!
 * Swapchain destruct is delayed until it is safe to destroy them, this function
 * does the actual destruction and is called from @ref
 * comp_swapchain_garbage_collect.
 *
 * @ingroup comp_util
 */
static void
really_destroy(struct comp_swapchain *sc)
{
	// Re-use close function.
	comp_swapchain_teardown(sc);

	free(sc);
}


/*
 *
 * 'Exported' parent-class functions.
 *
 */

xrt_result_t
comp_swapchain_create_init(struct comp_swapchain *sc,
                           comp_swapchain_destroy_func_t destroy_func,
                           struct vk_bundle *vk,
                           struct comp_swapchain_shared *cscs,
                           const struct xrt_swapchain_create_info *info,
                           const struct xrt_swapchain_create_properties *xsccp)
{
	VkResult ret;

	VK_DEBUG(vk, "CREATE %p %" PRIu32 "x%" PRIu32 " %s (%" PRId64 ")", //
	         (void *)sc,                                               //
	         info->width, info->height,                                //
	         vk_format_string(info->format), info->format);

	if ((info->create & XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT) != 0) {
		VK_WARN(vk,
		        "Swapchain info is valid but this compositor doesn't support creating protected content "
		        "swapchains!");
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	set_common_fields(sc, destroy_func, vk, cscs, xsccp->image_count);

	// Use the image helper to allocate the images.
	ret = vk_ic_allocate(vk, info, xsccp->image_count, &sc->vkic);
	if (ret == VK_ERROR_FEATURE_NOT_PRESENT) {
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}
	if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	if (ret != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

	xrt_graphics_buffer_handle_t handles[ARRAY_SIZE(sc->vkic.images)];

	vk_ic_get_handles(vk, &sc->vkic, ARRAY_SIZE(handles), handles);
	for (uint32_t i = 0; i < sc->vkic.image_count; i++) {
		sc->base.images[i].handle = handles[i];
		sc->base.images[i].size = sc->vkic.images[i].size;
		sc->base.images[i].use_dedicated_allocation = sc->vkic.images[i].use_dedicated_allocation;
	}

	do_post_create_vulkan_setup(vk, info, sc);

	return XRT_SUCCESS;
}

xrt_result_t
comp_swapchain_import_init(struct comp_swapchain *sc,
                           comp_swapchain_destroy_func_t destroy_func,
                           struct vk_bundle *vk,
                           struct comp_swapchain_shared *cscs,
                           const struct xrt_swapchain_create_info *info,
                           struct xrt_image_native *native_images,
                           uint32_t native_image_count)
{
	VkResult ret;

	VK_DEBUG(vk, "IMPORT %p %" PRIu32 "x%" PRIu32 " %s (%" PRId64 ")", //
	         (void *)sc,                                               //
	         info->width, info->height,                                //
	         vk_format_string(info->format), info->format);

	set_common_fields(sc, destroy_func, vk, cscs, native_image_count);

	// Use the image helper to get the images.
	ret = vk_ic_from_natives(vk, info, native_images, native_image_count, &sc->vkic);
	if (ret != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

	do_post_create_vulkan_setup(vk, info, sc);

	return XRT_SUCCESS;
}

void
comp_swapchain_teardown(struct comp_swapchain *sc)
{
	struct vk_bundle *vk = sc->vk;

	VK_TRACE(vk, "REALLY DESTROY");

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		// compositor ensures to garbage collect after gpu work finished
		if (sc->images[i].use_count != 0) {
			VK_ERROR(vk, "swapchain destroy while image %d use count %d", i, sc->images[i].use_count);
			assert(false);
			continue; // leaking better than crashing?
		}

		os_mutex_destroy(&sc->images[i].use_mutex);
		pthread_cond_destroy(&sc->images[i].use_cond);
	}

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		image_cleanup(vk, &sc->images[i]);
	}

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		u_graphics_buffer_unref(&sc->base.images[i].handle);
	}

	vk_ic_destroy(vk, &sc->vkic);
}


/*
 *
 * 'Exported' shared functions.
 *
 */

XRT_CHECK_RESULT xrt_result_t
comp_swapchain_shared_init(struct comp_swapchain_shared *cscs, struct vk_bundle *vk)
{
	VkResult ret = vk_cmd_pool_init(vk, &cscs->pool, 0);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_init: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

void
comp_swapchain_shared_destroy(struct comp_swapchain_shared *cscs, struct vk_bundle *vk)
{
	vk_cmd_pool_destroy(vk, &cscs->pool);
}

void
comp_swapchain_shared_garbage_collect(struct comp_swapchain_shared *cscs)
{
	struct comp_swapchain *sc;

	while ((sc = u_threading_stack_pop(&cscs->destroy_swapchains))) {
		sc->real_destroy(sc);
	}
}


/*
 *
 * 'Exported' default implementation.
 *
 */

xrt_result_t
comp_swapchain_get_create_properties(const struct xrt_swapchain_create_info *info,
                                     struct xrt_swapchain_create_properties *xsccp)
{
	uint32_t image_count = 3;

	if ((info->create & XRT_SWAPCHAIN_CREATE_STATIC_IMAGE) != 0) {
		image_count = 1;
	}

	U_ZERO(xsccp);
	xsccp->image_count = image_count;
	xsccp->extra_bits = XRT_SWAPCHAIN_USAGE_SAMPLED;

	return XRT_SUCCESS;
}

xrt_result_t
comp_swapchain_create(struct vk_bundle *vk,
                      struct comp_swapchain_shared *cscs,
                      const struct xrt_swapchain_create_info *info,
                      const struct xrt_swapchain_create_properties *xsccp,
                      struct xrt_swapchain **out_xsc)
{
	struct comp_swapchain *sc = U_TYPED_CALLOC(struct comp_swapchain);
	xrt_result_t xret;

	xret = comp_swapchain_create_init( //
	    sc,                            //
	    really_destroy,                //
	    vk,                            //
	    cscs,                          //
	    info,                          //
	    xsccp);                        //
	if (xret != XRT_SUCCESS) {
		free(sc);
		return xret;
	}

	// Correctly setup refcounts.
	xrt_swapchain_reference(out_xsc, &sc->base.base);

	return xret;
}

xrt_result_t
comp_swapchain_import(struct vk_bundle *vk,
                      struct comp_swapchain_shared *cscs,
                      const struct xrt_swapchain_create_info *info,
                      struct xrt_image_native *native_images,
                      uint32_t native_image_count,
                      struct xrt_swapchain **out_xsc)
{
	struct comp_swapchain *sc = U_TYPED_CALLOC(struct comp_swapchain);
	xrt_result_t xret;

	xret = comp_swapchain_import_init( //
	    sc,                            //
	    really_destroy,                //
	    vk,                            //
	    cscs,                          //
	    info,                          //
	    native_images,                 //
	    native_image_count);           //
	if (xret != XRT_SUCCESS) {
		free(sc);
		return xret;
	}

	// Correctly setup refcounts.
	xrt_swapchain_reference(out_xsc, &sc->base.base);

	return xret;
}
