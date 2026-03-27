#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo.h>
#include <xkbcommon/xkbcommon.h>
#include "escreen.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define HANDLE_SIZE 8

static void noop() {}

static int create_shm_file() {
	int fd = memfd_create("escreen-shm", MFD_CLOEXEC);
	if (fd < 0) return -1;
	return fd;
}

static void finalize_buffer(struct pool_buffer *pool) {
	if (pool->buffer) wl_buffer_destroy(pool->buffer);
	if (pool->cairo) cairo_destroy(pool->cairo);
	if (pool->cairo_surface) cairo_surface_destroy(pool->cairo_surface);
	if (pool->pixman) pixman_image_unref(pool->pixman);
	if (pool->data) munmap(pool->data, pool->size);
	memset(pool, 0, sizeof(*pool));
}

// Forward declaration needed since buffer_handle_release calls render()
static void render(struct escreen_output *output);
static void set_cursor(struct escreen_seat *seat, struct wl_pointer *pointer, uint32_t serial, const char *name);

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	(void)wl_buffer;
	struct pool_buffer *pool = data;
	pool->busy = false;
	if (pool->owner && pool->owner->dirty) {
		render(pool->owner);
	}
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

static struct pool_buffer *get_next_buffer(struct escreen_state *state, struct escreen_output *output, 
	int32_t width, int32_t height) {
	
	struct pool_buffer *pool = NULL;
	for (size_t i = 0; i < 3; i++) {
		if (output->buffers[i].busy) continue;
		pool = &output->buffers[i];
	}

	if (!pool) return NULL;

	if (pool->buffer && pool->width == width && pool->height == height) {
		return pool;
	}
	
	finalize_buffer(pool);

	const enum wl_shm_format wl_fmt = WL_SHM_FORMAT_XRGB8888;
	const cairo_format_t cairo_fmt = CAIRO_FORMAT_RGB24;
	uint32_t stride = cairo_format_stride_for_width(cairo_fmt, width);
	size_t size = (size_t)stride * height;

	int fd = create_shm_file();
	if (fd < 0) return NULL;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *wl_pool = wl_shm_create_pool(state->shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(wl_pool, 0, width, height, stride, wl_fmt);
	wl_buffer_add_listener(buffer, &buffer_listener, pool);
	wl_shm_pool_destroy(wl_pool);
	close(fd);

	pool->buffer = buffer;
	pool->owner = output;
	pool->data = data;
	pool->size = size;
	pool->width = width;
	pool->height = height;
	pool->stride = stride;
	pool->pixman = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height, data, stride);
	pool->cairo_surface = cairo_image_surface_create_for_data(data, cairo_fmt, width, height, stride);
	pool->cairo = cairo_create(pool->cairo_surface);

	return pool;
}

static void draw_handle(cairo_t *cr, double x, double y) {
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, x - HANDLE_SIZE/2, y - HANDLE_SIZE/2, HANDLE_SIZE, HANDLE_SIZE);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static const struct wl_callback_listener frame_listener;

static void render(struct escreen_output *output) {
	if (!output->surface || !output->configured) return;
	struct escreen_state *state = output->state;
	
	int32_t b_w = output->width * output->scale;
	int32_t b_h = output->height * output->scale;

	struct pool_buffer *pool = get_next_buffer(state, output, b_w, b_h);
	if (!pool) return;
	pool->busy = true;

	// 1. CLEAR STICKY CLIP & BUFFER
	memset(pool->data, 0, pool->size);
	pixman_image_set_clip_region32(pool->pixman, NULL);

	// 2. DARKENED BACKGROUND (OVERWRITE EVERYTHING)
	if (output->darkened_image) {
		pixman_image_set_repeat(output->darkened_image, PIXMAN_REPEAT_PAD);
		pixman_f_transform_t f_trans;
		pixman_f_transform_init_scale(&f_trans, 
			(double)pixman_image_get_width(output->darkened_image) / b_w,
			(double)pixman_image_get_height(output->darkened_image) / b_h);
		pixman_transform_t trans;
		pixman_transform_from_pixman_f_transform(&trans, &f_trans);
		pixman_image_set_transform(output->darkened_image, &trans);

		pixman_image_composite(PIXMAN_OP_SRC, output->darkened_image, NULL, pool->pixman, 0, 0, 0, 0, 0, 0, b_w, b_h);
	}

	struct escreen_seat *seat = wl_container_of(state->seats.next, seat, link);
	
	// 3. CAIRO RENDERING
	cairo_surface_mark_dirty(pool->cairo_surface);
	cairo_t *cr = pool->cairo;
	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_reset_clip(cr);
	cairo_scale(cr, output->scale, output->scale);
	cairo_translate(cr, -output->logical_geometry.x, -output->logical_geometry.y);

	if (seat->has_selection) {
		// Global seamless reveal from shared in-memory capture
		cairo_surface_t *global_capture = state->global_capture;

		if (global_capture && cairo_surface_status(global_capture) == CAIRO_STATUS_SUCCESS) {
			cairo_save(cr);
			// Place the global capture at its correct logical global offset
			cairo_set_source_surface(cr, global_capture, state->total_min_x, state->total_min_y);
			cairo_rectangle(cr, state->result.x, state->result.y, state->result.width, state->result.height);
			cairo_fill(cr);
			
			// Exact same behavior for the baked history layer
			if (state->sketching.history_layer) {
				cairo_set_source_surface(cr, state->sketching.history_layer, state->total_min_x, state->total_min_y);
				cairo_rectangle(cr, state->result.x, state->result.y, state->result.width, state->result.height);
				cairo_fill(cr);
			}
			cairo_restore(cr);
		}

		// Draw sketching tools
		tools_draw(state, cr);
		// Draw toolbar/UI for tools
		tools_draw_ui(state, cr);

		double l_x = (double)state->result.x;
		double l_y = (double)state->result.y;
		double l_w = (double)state->result.width;
		double l_h = (double)state->result.height;

		cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, l_x, l_y, l_w, l_h);
		cairo_stroke(cr);

		// Draw handles
		draw_handle(cr, l_x, l_y);
		draw_handle(cr, l_x + l_w, l_y);
		draw_handle(cr, l_x, l_y + l_h);
		draw_handle(cr, l_x + l_w, l_y + l_h);
		draw_handle(cr, l_x + l_w/2, l_y);
		draw_handle(cr, l_x + l_w/2, l_y + l_h);
		draw_handle(cr, l_x, l_y + l_h/2);
		draw_handle(cr, l_x + l_w, l_y + l_h/2);
	}
	cairo_restore(cr);
	cairo_surface_flush(pool->cairo_surface);

	if (output->frame_callback) wl_callback_destroy(output->frame_callback);
	output->frame_callback = wl_surface_frame(output->surface);
	wl_callback_add_listener(output->frame_callback, &frame_listener, output);

	// DAMAGE AND COMMIT LIKE SLURP
	wl_surface_attach(output->surface, pool->buffer, 0, 0);
	wl_surface_damage(output->surface, 0, 0, output->width, output->height);
	wl_surface_damage_buffer(output->surface, 0, 0, b_w, b_h);
	wl_surface_set_buffer_scale(output->surface, output->scale);
	wl_surface_commit(output->surface);
	output->dirty = false;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	(void)time;
	struct escreen_output *output = data;
	wl_callback_destroy(callback);
	output->frame_callback = NULL;
	if (output->dirty) render(output);
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void set_output_dirty(struct escreen_output *output) {
	output->dirty = true;
	if (output->frame_callback) return;
	render(output);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct escreen_output *output = data;
	output->width = width;
	output->height = height;
	output->configured = true;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	
	if (output->state->running) {
		set_output_dirty(output);
	}
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = (void*)noop,
};

static handle_type_t get_handle_at(struct escreen_box box, int32_t x, int32_t y) {
	int32_t h = HANDLE_SIZE / 2 + 4;
	if (abs(x - box.x) < h && abs(y - box.y) < h) return HANDLE_TOP_LEFT;
	if (abs(x - (box.x + box.width)) < h && abs(y - box.y) < h) return HANDLE_TOP_RIGHT;
	if (abs(x - box.x) < h && abs(y - (box.y + box.height)) < h) return HANDLE_BOTTOM_LEFT;
	if (abs(x - (box.x + box.width)) < h && abs(y - (box.y + box.height)) < h) return HANDLE_BOTTOM_RIGHT;
	if (abs(x - (box.x + box.width/2)) < h && abs(y - box.y) < h) return HANDLE_TOP;
	if (abs(x - (box.x + box.width/2)) < h && abs(y - (box.y + box.height)) < h) return HANDLE_BOTTOM;
	if (abs(x - box.x) < h && abs(y - (box.y + box.height/2)) < h) return HANDLE_LEFT;
	if (abs(x - (box.x + box.width)) < h && abs(y - (box.y + box.height/2)) < h) return HANDLE_RIGHT;
	return HANDLE_NONE;
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
	struct escreen_seat *seat = data;
	struct escreen_output *output;
	seat->pointer_serial = serial;
	wl_list_for_each(output, &seat->state->outputs, link) {
		if (output->surface == surface) {
			seat->current_output = output;
			seat->x = wl_fixed_to_int(sx) + output->logical_geometry.x;
			seat->y = wl_fixed_to_int(sy) + output->logical_geometry.y;
			break;
		}
	}
	set_cursor(seat, pointer, serial, "crosshair");
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface) {
	struct escreen_seat *seat = data;
	(void)pointer; (void)serial; (void)surface;
	seat->current_output = NULL;
}

static void update_dirty_outputs(struct escreen_seat *seat) {
	struct escreen_output *output;
	wl_list_for_each(output, &seat->state->outputs, link) {
		if (!output->surface) continue;
		set_output_dirty(output);
	}
}

static void set_cursor(struct escreen_seat *seat, struct wl_pointer *pointer, uint32_t serial, const char *name) {
	struct wl_cursor *cursor = wl_cursor_theme_get_cursor(seat->state->cursor_theme, name);
	if (!cursor) cursor = wl_cursor_theme_get_cursor(seat->state->cursor_theme, "left_ptr");
	if (cursor) {
		struct wl_cursor_image *image = cursor->images[0];
		struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
		wl_surface_attach(seat->state->cursor_surface, buffer, 0, 0);
		wl_surface_damage_buffer(seat->state->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_commit(seat->state->cursor_surface);
		if (pointer) wl_pointer_set_cursor(pointer, serial, seat->state->cursor_surface, image->hotspot_x, image->hotspot_y);
	}
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy) {
	struct escreen_seat *seat = data;
	(void)time;
	if (!seat->current_output) return;

	int32_t last_x = seat->x;
	int32_t last_y = seat->y;
	seat->x = wl_fixed_to_int(sx) + seat->current_output->logical_geometry.x;
	seat->y = wl_fixed_to_int(sy) + seat->current_output->logical_geometry.y;
	int32_t dx = seat->x - last_x;
	int32_t dy = seat->y - last_y;

	struct escreen_state *state = seat->state;

	if (seat->selection_status >= SELECTION_EDITING && !seat->button_pressed) {
		// Update cursor for handles
		handle_type_t hover = get_handle_at(state->result, seat->x, seat->y);
		const char *cname = "crosshair";
		switch (hover) {
			case HANDLE_TOP_LEFT: cname = "nw-resize"; break;
			case HANDLE_TOP_RIGHT: cname = "ne-resize"; break;
			case HANDLE_BOTTOM_LEFT: cname = "sw-resize"; break;
			case HANDLE_BOTTOM_RIGHT: cname = "se-resize"; break;
			case HANDLE_TOP: cname = "n-resize"; break;
			case HANDLE_BOTTOM: cname = "s-resize"; break;
			case HANDLE_LEFT: cname = "w-resize"; break;
			case HANDLE_RIGHT: cname = "e-resize"; break;
			case HANDLE_NONE:
				if (seat->x >= state->result.x && seat->x < state->result.x + state->result.width &&
					seat->y >= state->result.y && seat->y < state->result.y + state->result.height) {
					cname = "move"; // or pointer
				}
				break;
		}
		
		if (tools_is_on_toolbar(state, seat->x, seat->y)) {
			cname = "default"; // Pointer/Arrow over toolbar
		}
		
		set_cursor(seat, pointer, seat->pointer_serial, cname);
	}

	if (seat->selection_status == SELECTION_DRAGGING) {
		int32_t x1 = seat->anchor_x;
		int32_t y1 = seat->anchor_y;
		int32_t x2 = seat->x;
		int32_t y2 = seat->y;
		state->result.x = (x1 < x2) ? x1 : x2;
		state->result.y = (y1 < y2) ? y1 : y2;
		state->result.width = abs(x2 - x1);
		state->result.height = abs(y2 - y1);
		update_dirty_outputs(seat);
	} else if (seat->selection_status == SELECTION_MOVING) {
		state->result.x += dx;
		state->result.y += dy;
		update_dirty_outputs(seat);
	} else if (seat->selection_status == SELECTION_RESIZING) {
		struct escreen_box *box = &state->result;
		switch (seat->active_handle) {
			case HANDLE_TOP_LEFT: box->x += dx; box->y += dy; box->width -= dx; box->height -= dy; break;
			case HANDLE_TOP_RIGHT: box->y += dy; box->width += dx; box->height -= dy; break;
			case HANDLE_BOTTOM_LEFT: box->x += dx; box->width -= dx; box->height += dy; break;
			case HANDLE_BOTTOM_RIGHT: box->width += dx; box->height += dy; break;
			case HANDLE_TOP: box->y += dy; box->height -= dy; break;
			case HANDLE_BOTTOM: box->height += dy; break;
			case HANDLE_LEFT: box->x += dx; box->width -= dx; break;
			case HANDLE_RIGHT: box->width += dx; break;
			default: break;
		}
		if (box->width < 0) {
			box->x += box->width;
			box->width = -box->width;
			switch (seat->active_handle) {
				case HANDLE_TOP_LEFT: seat->active_handle = HANDLE_TOP_RIGHT; break;
				case HANDLE_TOP_RIGHT: seat->active_handle = HANDLE_TOP_LEFT; break;
				case HANDLE_BOTTOM_LEFT: seat->active_handle = HANDLE_BOTTOM_RIGHT; break;
				case HANDLE_BOTTOM_RIGHT: seat->active_handle = HANDLE_BOTTOM_LEFT; break;
				case HANDLE_LEFT: seat->active_handle = HANDLE_RIGHT; break;
				case HANDLE_RIGHT: seat->active_handle = HANDLE_LEFT; break;
				default: break;
			}
		}
		if (box->height < 0) {
			box->y += box->height;
			box->height = -box->height;
			switch (seat->active_handle) {
				case HANDLE_TOP_LEFT: seat->active_handle = HANDLE_BOTTOM_LEFT; break;
				case HANDLE_BOTTOM_LEFT: seat->active_handle = HANDLE_TOP_LEFT; break;
				case HANDLE_TOP_RIGHT: seat->active_handle = HANDLE_BOTTOM_RIGHT; break;
				case HANDLE_BOTTOM_RIGHT: seat->active_handle = HANDLE_TOP_RIGHT; break;
				case HANDLE_TOP: seat->active_handle = HANDLE_BOTTOM; break;
				case HANDLE_BOTTOM: seat->active_handle = HANDLE_TOP; break;
				default: break;
			}
		}
		update_dirty_outputs(seat);
	} else if (seat->selection_status == SELECTION_EDITING) {
		tools_handle_motion(state, seat->x, seat->y);
		update_dirty_outputs(seat);
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state_action) {
	struct escreen_seat *seat = data;
	(void)pointer; (void)time;
	seat->pointer_serial = serial;

	if (state_action == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (button == BTN_LEFT) {
			if (seat->selection_status == SELECTION_NOT_STARTED) {
				seat->anchor_x = seat->x;
				seat->anchor_y = seat->y;
				seat->selection_status = SELECTION_DRAGGING;
				seat->has_selection = true;
			} else if (seat->has_selection) {
				// Toolbar hit test MUST come first
				if (tools_is_on_toolbar(seat->state, seat->x, seat->y)) {
					tools_handle_button(seat->state, seat->x, seat->y, true);
					update_dirty_outputs(seat);
					return;
				}
				
				handle_type_t handle = get_handle_at(seat->state->result, seat->x, seat->y);
				if (handle != HANDLE_NONE) {
					seat->selection_status = SELECTION_RESIZING;
					seat->active_handle = handle;
				} else if (seat->x >= seat->state->result.x && seat->x < seat->state->result.x + seat->state->result.width &&
						   seat->y >= seat->state->result.y && seat->y < seat->state->result.y + seat->state->result.height) {
					if (seat->state->sketching.active_tool == NULL) { // TOOL_SELECT
						seat->selection_status = SELECTION_MOVING;
					} else {
						tools_handle_button(seat->state, seat->x, seat->y, true);
					}
				} else {
					// Clicked outside, reset
					seat->has_selection = false;
					seat->selection_status = SELECTION_NOT_STARTED;
					update_dirty_outputs(seat);
				}
			}
		} else if (button == BTN_RIGHT) {
			if (seat->has_selection && seat->selection_status != SELECTION_DRAGGING) {
				// Find nearest corner
				struct escreen_box b = seat->state->result;
				double dx0 = seat->x - b.x; double dy0 = seat->y - b.y;
				double dx1 = seat->x - (b.x + b.width); double dy1 = seat->y - b.y;
				double dx2 = seat->x - b.x; double dy2 = seat->y - (b.y + b.height);
				double dx3 = seat->x - (b.x + b.width); double dy3 = seat->y - (b.y + b.height);
				double dists[4] = { dx0*dx0+dy0*dy0, dx1*dx1+dy1*dy1, dx2*dx2+dy2*dy2, dx3*dx3+dy3*dy3 };
				double min_dist = dists[0];
				handle_type_t best = HANDLE_TOP_LEFT;
				if (dists[1] < min_dist) { min_dist=dists[1]; best=HANDLE_TOP_RIGHT; }
				if (dists[2] < min_dist) { min_dist=dists[2]; best=HANDLE_BOTTOM_LEFT; }
				if (dists[3] < min_dist) { min_dist=dists[3]; best=HANDLE_BOTTOM_RIGHT; }
				
				seat->selection_status = SELECTION_RESIZING;
				seat->active_handle = best;
			}
		}
	} else {
		// Released
		if (button == BTN_LEFT || button == BTN_RIGHT) {
			if (seat->selection_status == SELECTION_DRAGGING || 
				seat->selection_status == SELECTION_MOVING || 
				seat->selection_status == SELECTION_RESIZING) {
				seat->selection_status = SELECTION_EDITING;
				update_dirty_outputs(seat);
			} else if (seat->selection_status == SELECTION_EDITING) {
				if (button == BTN_LEFT) {
					tools_handle_button(seat->state, seat->x, seat->y, false);
					update_dirty_outputs(seat);
				}
			}
		}
	}
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct escreen_seat *seat = data;
	(void)keyboard;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}
	seat->xkb.keymap = xkb_keymap_new_from_string(seat->state->xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_str, size);
	close(fd);
	seat->xkb.state = xkb_state_new(seat->xkb.keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	(void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct escreen_seat *seat = data;
	(void)keyboard; (void)serial; (void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

	xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->xkb.state, key + 8);
	if (sym == XKB_KEY_Return || sym == XKB_KEY_d) {
		if (seat->has_selection) seat->state->running = false;
	} else if (sym == XKB_KEY_Escape) {
		seat->state->result.width = 0;
		seat->state->running = false;
	} else if (sym == XKB_KEY_c && (xkb_state_mod_name_is_active(seat->xkb.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))) {
		if (seat->has_selection) seat->state->running = false;
	} else if ((sym == XKB_KEY_z || sym == XKB_KEY_Z) && (xkb_state_mod_name_is_active(seat->xkb.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))) {
		if (xkb_state_mod_name_is_active(seat->xkb.state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) {
			if (seat->state->sketching.history_undo_pos < seat->state->sketching.history_count) {
				seat->state->sketching.history_undo_pos++;
				update_dirty_outputs(seat);
			}
		} else {
			if (seat->state->sketching.history_undo_pos > 0) {
				seat->state->sketching.history_undo_pos--;
				update_dirty_outputs(seat);
			}
		}
	} else if (sym == XKB_KEY_y && (xkb_state_mod_name_is_active(seat->xkb.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))) {
		if (seat->state->sketching.history_undo_pos < seat->state->sketching.history_count) {
			seat->state->sketching.history_undo_pos++;
			update_dirty_outputs(seat);
		}
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct escreen_seat *seat = data;
	(void)keyboard; (void)serial;
	xkb_state_update_mask(seat->xkb.state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = (void*)noop,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = (void*)noop,
};

const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = (void*)noop,
	.frame = (void*)noop,
	.axis_source = (void*)noop,
	.axis_stop = (void*)noop,
	.axis_discrete = (void*)noop,
};

void selection_run(struct escreen_state *state) {
	struct escreen_output *output;
	wl_list_for_each(output, &state->outputs, link) {
		output->surface = wl_compositor_create_surface(state->compositor);
		output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(state->layer_shell,
			output->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "escreen");
		
		zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_surface_listener, output);
		zwlr_layer_surface_v1_set_anchor(output->layer_surface, 
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer_surface, 1);
		zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
		
		wl_surface_commit(output->surface);
	}

	while (state->running && wl_display_dispatch(state->display) != -1) {
	}

	wl_list_for_each(output, &state->outputs, link) {
		if (output->frame_callback) wl_callback_destroy(output->frame_callback);
		output->frame_callback = NULL;
		if (output->layer_surface) zwlr_layer_surface_v1_destroy(output->layer_surface);
		if (output->surface) wl_surface_destroy(output->surface);
		output->layer_surface = NULL;
		output->surface = NULL;
	}
	wl_display_roundtrip(state->display);

	struct escreen_seat *seat;
	wl_list_for_each(seat, &state->seats, link) {
		if (seat->xkb.state) xkb_state_unref(seat->xkb.state);
		if (seat->xkb.keymap) xkb_keymap_unref(seat->xkb.keymap);
	}
}
