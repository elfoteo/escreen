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
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "escreen.h"
#include "imgui_impl_opengl3.h"
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

			// Compute the true physical/logical pixel ratio.
			if (output->logical_geometry.width > 0) {
				output->scale_factor = (double)fw / output->logical_geometry.width;
			} else {
				output->scale_factor = (double)output->scale;
			}

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

			pixman_image_set_destroy_function(dark, (void*)free, dark_data);
			
			// 1. Copy original (using SRC operator)
			pixman_image_composite(PIXMAN_OP_SRC, 
				output->frozen_buffer.pixman, NULL, dark,
				0, 0, 0, 0, 0, 0, fw, fh);
			
			// 2. DARKEN (Blend with 40% black)
			pixman_color_t black = {0, 0, 0, 0x6666};
			pixman_image_t *solid = pixman_image_create_solid_fill(&black);
			pixman_image_composite(PIXMAN_OP_OVER,
				solid, NULL, dark,
				0, 0, 0, 0, 0, 0, fw, fh);
			pixman_image_unref(solid);
			output->darkened_image = dark;

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

	// Find the maximum scale_factor across all outputs for high-res stitching.
	double max_scale_factor = 1.0;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->scale_factor > max_scale_factor) max_scale_factor = output->scale_factor;
	}
	state->max_scale_factor = max_scale_factor;

	if (!first) {
		int tw = (int)round((max_x - min_x) * max_scale_factor);
		int th = (int)round((max_y - min_y) * max_scale_factor);
		cairo_surface_t *total = cairo_image_surface_create(CAIRO_FORMAT_RGB24, tw, th);
		cairo_t *cr = cairo_create(total);
		cairo_scale(cr, max_scale_factor, max_scale_factor);
		cairo_translate(cr, -min_x, -min_y);
		
		wl_list_for_each(output, &state->outputs, link) {
			if (output->frozen_failed || !output->frozen_buffer.cairo_surface) continue;
            cairo_save(cr);
            cairo_identity_matrix(cr);
            cairo_translate(cr, 
                round((output->logical_geometry.x - min_x) * max_scale_factor),
                round((output->logical_geometry.y - min_y) * max_scale_factor));
            double s_ratio = max_scale_factor / output->scale_factor;
            cairo_scale(cr, s_ratio, s_ratio);
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

	int max_scale = 1;
	struct escreen_output *o;
	wl_list_for_each(o, &state->outputs, link) {
		if (o->scale > max_scale) max_scale = o->scale;
	}

	int32_t pw = state->result.width * max_scale;
	int32_t ph = state->result.height * max_scale;

	// 1. Setup Offscreen FBO
	// Make sure we have a current context (reuse from first output or state)
	// For simplicity, we assume selection_run has already initialized everything.
	
	GLuint fbo, tex;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

	glViewport(0, 0, pw, ph);
	glClearColor(0,0,0,1);
	glClear(GL_COLOR_BUFFER_BIT);

	// 2. Render background screenshot
	// We'll use a simplified version of the selection.c shader logic here
	// Or better: call a shared function if we had one.
	// For now, let's just do it directly.
	
	// (Reusing g_shader and g_vbo from selection.c would be ideal, but they are static there)
	// Let's just render the sketches for now to make sure the interface works, 
	// and add background logic next.
	
	ImGui::NewFrame();
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	
	// Translate all drawing to be relative to the crop area
	draw->PushClipRect(ImVec2(0,0), ImVec2((float)pw, (float)ph));
	
	// Draw the screenshot onto the ImDrawList or directly
	// Actually, drawing the texture via ImDrawList is easiest!
	int32_t total_w = state->total_max_x - state->total_min_x;
	int32_t total_h = state->total_max_y - state->total_min_y;
	
	float u0 = (float)(state->result.x - state->total_min_x) / total_w;
	float v0 = (float)(state->result.y - state->total_min_y) / total_h;
	float u1 = (float)(state->result.x + state->result.width - state->total_min_x) / total_w;
	float v1 = (float)(state->result.y + state->result.height - state->total_min_y) / total_h;
	
	draw->AddImage((ImTextureID)(intptr_t)state->global_capture_tex, ImVec2(0,0), ImVec2((float)pw, (float)ph), ImVec2(u0,v0), ImVec2(u1,v1));

	// Draw sketches
	// We need to scale the sketches by max_scale and offset by -state->result.x/y
	// ImDrawList has a transformation stack, or we can just apply it in tools_draw.
	// Actually, let's just use a Push/Pop offset if ImGui supported it (it doesn't easily for whole list).
	// We'll just temporarily adjust the state offset.
	
	int32_t old_min_x = state->total_min_x;
	int32_t old_min_y = state->total_min_y;
	state->total_min_x = state->result.x;
	state->total_min_y = state->result.y;
	
	// tools_draw needs to be aware of the scale too. 
	// We'll just assume points are in logical coordinates.
	tools_draw(state, draw);
	
	state->total_min_x = old_min_x;
	state->total_min_y = old_min_y;

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	// 3. Read pixels
	uint32_t *pixels = malloc(pw * ph * 4);
	glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	
	// OpenGL reads upside down compared to Cairo/PNG
	uint32_t *flipped = malloc(pw * ph * 4);
	for (int y = 0; y < ph; y++) {
		memcpy(flipped + y * pw, pixels + (ph - 1 - y) * pw, pw * 4);
	}

	image_save(state, flipped, pw, ph, pw * 4);

	free(pixels);
	free(flipped);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
}
