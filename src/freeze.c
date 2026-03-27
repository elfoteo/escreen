#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <wayland-client.h>
#include <cairo.h>
#include "escreen.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

static int create_shm_file() {
	int fd = memfd_create("escreen-shm", MFD_CLOEXEC);
	if (fd < 0) return -1;
	return fd;
}

static void frame_handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct escreen_output *output = data;
	(void)format; (void)frame;

	struct pool_buffer *pool = &output->frozen_buffer;
	if (pool->buffer) return;

	pool->width = width;
	pool->height = height;
	pool->stride = stride;
	pool->size = stride * height;

	int fd = create_shm_file();
	if (fd < 0) return;
	if (ftruncate(fd, pool->size) < 0) {
		close(fd);
		return;
	}

	pool->data = mmap(NULL, pool->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pool->data == MAP_FAILED) {
		close(fd);
		return;
	}

	struct wl_shm_pool *wl_pool = wl_shm_create_pool(output->state->shm, fd, pool->size);
	pool->buffer = wl_shm_pool_create_buffer(wl_pool, 0, width, height, stride, format);
	wl_shm_pool_destroy(wl_pool);
	close(fd);
}

static void frame_handle_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	struct escreen_output *output = data;
	if (output->frozen_buffer.buffer) {
		zwlr_screencopy_frame_v1_copy(frame, output->frozen_buffer.buffer);
	}
}

static void frame_handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	(void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
	struct escreen_output *output = data;
	output->frozen_captured = true;
	output->frozen_failed = false;
}

static void frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	(void)frame;
	struct escreen_output *output = data;
	output->frozen_captured = true;
	output->frozen_failed = true;
}

static void noop() {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_handle_buffer,
	.flags = (void*)noop,
	.ready = frame_handle_ready,
	.failed = frame_handle_failed,
	.damage = (void*)noop,
	.linux_dmabuf = (void*)noop,
	.buffer_done = frame_handle_buffer_done,
};

void freeze_run(struct escreen_state *state) {
	struct escreen_output *output;
	int pending = 0;
	wl_list_for_each(output, &state->outputs, link) {
		struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(
			state->screencopy_manager, 0, output->wl_output);
		
		zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, output);
		pending++;
	}

	while (pending > 0 && wl_display_dispatch(state->display) != -1) {
		pending = 0;
		wl_list_for_each(output, &state->outputs, link) {
			if (!output->frozen_captured) pending++;
		}
	}

	wl_list_for_each(output, &state->outputs, link) {
		if (!output->frozen_failed && output->frozen_buffer.data) {
			int fw = output->frozen_buffer.width;
			int fh = output->frozen_buffer.height;
			
			// Original frozen image
			output->frozen_buffer.pixman = pixman_image_create_bits(
				PIXMAN_x8r8g8b8, fw, fh, output->frozen_buffer.data, output->frozen_buffer.stride);
			
			// Pre-bake darkened background
			int dark_stride = fw * 4;
			uint32_t *dark_data = calloc(fh, dark_stride);
			pixman_image_t *dark = pixman_image_create_bits(
				PIXMAN_x8r8g8b8, fw, fh, dark_data, dark_stride);
			
			if (!dark) {
				free(dark_data);
				continue;
			}

			// Ensure the image data is freed when the image is destroyed
			pixman_image_set_destroy_function(dark, (void*)free, dark_data);
			
			// 1. Copy original (using SRC operator)
			pixman_image_composite(PIXMAN_OP_SRC, 
				output->frozen_buffer.pixman, NULL, dark,
				0, 0, 0, 0, 0, 0, fw, fh);
			
			// 2. DARKEN (Blend with 40% black)
			pixman_color_t black = {0, 0, 0, 0x6666}; // ~40% opacity (0xFFFF is 100%)
			pixman_image_t *solid = pixman_image_create_solid_fill(&black);
			
			pixman_image_composite(PIXMAN_OP_OVER,
				solid, NULL, dark,
				0, 0, 0, 0, 0, 0, fw, fh);
			
			pixman_image_unref(solid);
			output->darkened_image = dark;

			// Fallback for parts of the code still using Cairo (like crop_and_save for now)
			output->frozen_buffer.cairo_surface = cairo_image_surface_create_for_data(
				output->frozen_buffer.data, CAIRO_FORMAT_RGB24,
				fw, fh, output->frozen_buffer.stride);
		}
	}

	// 3. BUILD SEAMLESS DESKTOP IMAGE
	int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
	bool first = true;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->frozen_failed || !output->frozen_buffer.data) continue;
		if (first) {
			min_x = output->logical_geometry.x;
			min_y = output->logical_geometry.y;
			max_x = output->logical_geometry.x + output->logical_geometry.width;
			max_y = output->logical_geometry.y + output->logical_geometry.height;
			first = false;
		} else {
			if (output->logical_geometry.x < min_x) min_x = output->logical_geometry.x;
			if (output->logical_geometry.y < min_y) min_y = output->logical_geometry.y;
			if (output->logical_geometry.x + output->logical_geometry.width > max_x) 
                max_x = output->logical_geometry.x + output->logical_geometry.width;
			if (output->logical_geometry.y + output->logical_geometry.height > max_y) 
                max_y = output->logical_geometry.y + output->logical_geometry.height;
		}
	}

	state->total_min_x = min_x;
	state->total_min_y = min_y;
	state->total_max_x = max_x;
	state->total_max_y = max_y;

	// Determine the maximum scale across all outputs for high-res stitching
	int max_scale = 1;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->scale > max_scale) max_scale = output->scale;
	}

	if (!first) {
		int tw = (max_x - min_x) * max_scale;
		int th = (max_y - min_y) * max_scale;
		cairo_surface_t *total = cairo_image_surface_create(CAIRO_FORMAT_RGB24, tw, th);
		cairo_t *cr = cairo_create(total);
		cairo_scale(cr, (double)max_scale, (double)max_scale);
		cairo_translate(cr, -min_x, -min_y);
		
		wl_list_for_each(output, &state->outputs, link) {
			if (output->frozen_failed || !output->frozen_buffer.cairo_surface) continue;
            cairo_save(cr);
            cairo_translate(cr, output->logical_geometry.x, output->logical_geometry.y);
            cairo_scale(cr, 1.0/output->scale, 1.0/output->scale); 
            cairo_set_source_surface(cr, output->frozen_buffer.cairo_surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
		}
		
		state->global_capture = total;
		cairo_destroy(cr);
	}
}

void crop_and_save(struct escreen_state *state) {
	if (state->result.width <= 0 || state->result.height <= 0) return;

	// Use the in-memory seamless capture
	cairo_surface_t *source = state->global_capture;
	if (!source || cairo_surface_status(source) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Error: No global capture available in-memory\n");
		return;
	}

	// We need to know what scale the PNG was saved at
	int max_scale = 1;
	struct escreen_output *o;
	wl_list_for_each(o, &state->outputs, link) {
		if (o->scale > max_scale) max_scale = o->scale;
	}

	int32_t pw = state->result.width * max_scale;
	int32_t ph = state->result.height * max_scale;

	cairo_surface_t *dest = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
	cairo_t *cr = cairo_create(dest);
	
	// Composite from the clean global source
	cairo_set_source_surface(cr, source, 
		(state->total_min_x - state->result.x) * max_scale,
		(state->total_min_y - state->result.y) * max_scale);
	cairo_paint(cr);
	
	// Render sketching tools on the final output by directly executing vector actions
	cairo_save(cr);
	cairo_scale(cr, (double)max_scale, (double)max_scale);
	cairo_translate(cr, -state->result.x, -state->result.y);
	
	for (size_t i = 0; i < state->sketching.history_undo_pos; i++) {
		action_t *action = &state->sketching.history[i];
		if (state->sketching.tools[action->type] && state->sketching.tools[action->type]->render_action) {
			state->sketching.tools[action->type]->render_action(state, cr, action);
		}
	}
	
	if (state->sketching.drawing && state->sketching.active_tool && state->sketching.active_tool->draw_preview) {
		state->sketching.active_tool->draw_preview(state, cr);
	}
	
	cairo_restore(cr);

	cairo_surface_flush(dest);
	cairo_destroy(cr);

	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(dest);
	int stride = cairo_image_surface_get_stride(dest);
	image_save(state, pixels, pw, ph, stride);

	cairo_surface_destroy(dest);
}
