// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <string.h>
#include <xcb/render.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "kernel.h"
#include "log.h"
#include "win.h"
#include "x.h"

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
xcb_render_picture_t solid_picture(xcb_connection_t *c, xcb_drawable_t d, bool argb,
                                   double a, double r, double g, double b) {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t picture;
	xcb_render_create_picture_value_list_t pa;
	xcb_render_color_t col;
	xcb_rectangle_t rect;

	pixmap = x_create_pixmap(c, argb ? 32 : 8, d, 1, 1);
	if (!pixmap)
		return XCB_NONE;

	pa.repeat = 1;
	picture = x_create_picture_with_standard_and_pixmap(
	    c, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
	    XCB_RENDER_CP_REPEAT, &pa);

	if (!picture) {
		xcb_free_pixmap(c, pixmap);
		return XCB_NONE;
	}

	col.alpha = (uint16_t)(a * 0xffff);
	col.red = (uint16_t)(r * 0xffff);
	col.green = (uint16_t)(g * 0xffff);
	col.blue = (uint16_t)(b * 0xffff);

	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;

	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
	xcb_free_pixmap(c, pixmap);

	return picture;
}

xcb_image_t *make_shadow(xcb_connection_t *c, const conv *kernel, double opacity,
                         uint16_t width, uint16_t height) {
	/*
	 * We classify shadows into 4 kinds of regions
	 *    r = shadow radius
	 * (0, 0) is the top left of the window itself
	 *         -r     r      width-r  width+r
	 *       -r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 *        r +-----+---------+-----+
	 *          |  2  |    3    |  2  |
	 * height-r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 * height+r +-----+---------+-----+
	 */
	xcb_image_t *ximage;
	const double *shadow_sum = kernel->rsum;
	assert(shadow_sum);
	// We only support square kernels for shadow
	assert(kernel->w == kernel->h);
	uint d = (uint)kernel->w;
	uint16_t r = (uint16_t)d / 2;
	uint16_t swidth = width + r * 2, sheight = height + r * 2;

	assert(d % 2 == 1);
	assert(d > 0);

	ximage = xcb_image_create_native(c, swidth, sheight, XCB_IMAGE_FORMAT_Z_PIXMAP, 8,
	                                 0, 0, NULL);
	if (!ximage) {
		log_error("failed to create an X image");
		return 0;
	}

	unsigned char *data = ximage->data;
	uint32_t sstride = ximage->stride;

	// If the window body is smaller than the kernel, we do convolution directly
	if (width < r * 2 && height < r * 2) {
		for (uint y = 0; y < sheight; y++) {
			for (uint x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(
				    kernel, (int)(d - x - 1), (int)(d - y - 1), width, height);
				data[y * sstride + x] = (uint8_t)(sum * 255.0);
			}
		}
		return ximage;
	}

	if (height < r * 2) {
		// If the window height is smaller than the kernel, we divide
		// the window like this:
		// -r     r         width-r  width+r
		// +------+-------------+------+
		// |      |             |      |
		// +------+-------------+------+
		for (uint y = 0; y < sheight; y++) {
			for (uint x = 0; x < r * 2; x++) {
				double sum =
				    sum_kernel_normalized(kernel, (int)(d - x - 1),
				                          (int)(d - y - 1), d, height) *
				    255.0;
				data[y * sstride + x] = (uint8_t)sum;
				data[y * sstride + swidth - x - 1] = (uint8_t)sum;
			}
		}
		for (uint y = 0; y < sheight; y++) {
			double sum =
			    sum_kernel_normalized(kernel, 0, (int)(d - y - 1), d, height) * 255.0;
			memset(&data[y * sstride + r * 2], (uint8_t)sum, width - 2 * r);
		}
		return ximage;
	}
	if (width < r * 2) {
		// Similarly, for width smaller than kernel
		for (uint y = 0; y < r * 2; y++) {
			for (uint x = 0; x < swidth; x++) {
				double sum =
				    sum_kernel_normalized(kernel, (int)(d - x - 1),
				                          (int)(d - y - 1), width, d) *
				    255.0;
				data[y * sstride + x] = (uint8_t)sum;
				data[(sheight - y - 1) * sstride + x] = (uint8_t)sum;
			}
		}
		for (uint x = 0; x < swidth; x++) {
			double sum =
			    sum_kernel_normalized(kernel, (int)(d - x - 1), 0, width, d) * 255.0;
			for (uint y = r * 2; y < height; y++) {
				data[y * sstride + x] = (uint8_t)sum;
			}
		}
		return ximage;
	}

	// Fill part 3
	for (uint y = r; y < height + r; y++) {
		memset(data + sstride * y + r, (uint8_t)(255 * opacity), width);
	}

	// Part 1
	for (uint y = 0; y < r * 2; y++) {
		for (uint x = 0; x < r * 2; x++) {
			double tmpsum = shadow_sum[y * d + x] * opacity * 255.0;
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	// Part 2, top/bottom
	for (uint y = 0; y < r * 2; y++) {
		double tmpsum = shadow_sum[d * y + d - 1] * opacity * 255.0;
		memset(&data[y * sstride + r * 2], (uint8_t)tmpsum, width - r * 2);
		memset(&data[(sheight - y - 1) * sstride + r * 2], (uint8_t)tmpsum,
		       width - r * 2);
	}

	// Part 2, left/right
	for (uint x = 0; x < r * 2; x++) {
		double tmpsum = shadow_sum[d * (d - 1) + x] * opacity * 255.0;
		for (uint y = r * 2; y < height; y++) {
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
bool build_shadow(xcb_connection_t *c, xcb_drawable_t d, double opacity,
                  const uint16_t width, const uint16_t height, const conv *kernel,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict) {
	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	shadow_image = make_shadow(c, kernel, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap = x_create_pixmap(c, 8, d, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(c, 32, d, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb) {
		goto shadow_picture_err;
	}

	gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(c, XCB_RENDER_PICT_OP_SRC, shadow_pixel, shadow_picture,
	                     shadow_picture_argb, 0, 0, 0, 0, 0, 0, shadow_image->width,
	                     shadow_image->height);

	*pixmap = shadow_pixmap_argb;
	*pict = shadow_picture_argb;

	xcb_free_gc(c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(c, shadow_pixmap);
	xcb_render_free_picture(c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image) {
		xcb_image_destroy(shadow_image);
	}
	if (shadow_pixmap) {
		xcb_free_pixmap(c, shadow_pixmap);
	}
	if (shadow_pixmap_argb) {
		xcb_free_pixmap(c, shadow_pixmap_argb);
	}
	if (shadow_picture) {
		xcb_render_free_picture(c, shadow_picture);
	}
	if (shadow_picture_argb) {
		xcb_render_free_picture(c, shadow_picture_argb);
	}
	if (gc) {
		xcb_free_gc(c, gc);
	}

	return false;
}

void *
default_backend_render_shadow(backend_t *backend_data, uint width, uint height,
                              const conv *kernel, double r, double g, double b, double a) {
	xcb_pixmap_t shadow_pixel = solid_picture(backend_data->c, backend_data->root,
	                                          true, 1, r, g, b),
	             shadow = XCB_NONE;
	xcb_render_picture_t pict = XCB_NONE;

	assert(width < UINT16_MAX);
	assert(height < UINT16_MAX);
	build_shadow(backend_data->c, backend_data->root, a, (uint16_t)width,
	             (uint16_t)height, kernel, shadow_pixel, &shadow, &pict);

	auto visual = x_get_visual_for_standard(backend_data->c, XCB_PICT_STANDARD_ARGB_32);
	void *ret = backend_data->ops->bind_pixmap(
	    backend_data, shadow, x_get_visual_info(backend_data->c, visual), true);
	xcb_render_free_picture(backend_data->c, pict);
	return ret;
}

bool default_is_win_transparent(void *backend_data, win *w, void *win_data) {
	return w->mode != WMODE_SOLID;
}

bool default_is_frame_transparent(void *backend_data, win *w, void *win_data) {
	return w->frame_opacity != 1;
}
