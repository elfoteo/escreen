#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include "escreen.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

static void noop() {}

static void noop_done(void *data, struct wl_output *output) {
	(void)data; (void)output;
}

static void xdg_output_handle_logical_position(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
	(void)xdg_output;
	struct escreen_output *output = data;
	output->logical_geometry.x = x;
	output->logical_geometry.y = y;
}

static void xdg_output_handle_logical_size(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
	(void)xdg_output;
	struct escreen_output *output = data;
	output->logical_geometry.width = width;
	output->logical_geometry.height = height;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = (void*)noop,
	.name = (void*)noop,
	.description = (void*)noop,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		uint32_t capabilities);

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = (void*)noop,
};

extern const struct wl_pointer_listener pointer_listener;
extern const struct wl_keyboard_listener keyboard_listener;

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		uint32_t capabilities) {
	struct escreen_seat *seat = data;
	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	}
	if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		seat->wl_keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(seat->wl_keyboard, &keyboard_listener, seat);
	}
}

static void output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	(void)wl_output; (void)physical_width; (void)physical_height; (void)subpixel; (void)make; (void)model; (void)transform;
	struct escreen_output *output = data;
	output->geometry.x = x;
	output->geometry.y = y;
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	(void)wl_output; (void)flags; (void)refresh;
	struct escreen_output *output = data;
	output->geometry.width = width;
	output->geometry.height = height;
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	(void)wl_output;
	struct escreen_output *output = data;
	output->scale = scale;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = noop_done,
	.scale = output_handle_scale,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)version;
	struct escreen_state *state = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		state->screencopy_manager = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, 3);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 3);
	} else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
		state->data_control_manager = wl_registry_bind(registry, name,
			&zwlr_data_control_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output *wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 3);
		struct escreen_output *output = calloc(1, sizeof(struct escreen_output));
		output->wl_output = wl_output;
		output->state = state;
		output->scale = 1;
		wl_list_insert(&state->outputs, &output->link);
		wl_output_add_listener(wl_output, &output_listener, output);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		struct escreen_seat *seat = calloc(1, sizeof(struct escreen_seat));
		 seat->wl_seat = wl_seat;
		 seat->state = state;
		 wl_list_insert(&state->seats, &seat->link);
		 wl_seat_add_listener(wl_seat, &seat_listener, seat);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = (void*)noop,
};

int main(int argc, char **argv) {
	struct escreen_state state = {0};
	state.clipboard = true; // Default to clipboard
	wl_list_init(&state.outputs);
	wl_list_init(&state.seats);

	int c;
	int delay = 0;
	while ((c = getopt(argc, argv, "cfd:")) != -1) {
		switch (c) {
		case 'c':
			state.clipboard = true;
			state.save_file = false;
			break;
		case 'f':
			state.save_file = true;
			state.clipboard = false;
			break;
		case 'd':
			delay = atoi(optarg);
			break;
		default:
			break;
		}
	}

	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		fprintf(stderr, "Failed to connect to wayland display\n");
		return 1;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);

	wl_display_roundtrip(state.display);

	if (!state.compositor || !state.shm || !state.layer_shell || !state.screencopy_manager || !state.xdg_output_manager) {
		fprintf(stderr, "Compositor missing required protocols\n");
		return 1;
	}

	// Initialize xkb and cursor
	state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state.cursor_theme = wl_cursor_theme_load(NULL, 24, state.shm);
	if (!state.cursor_theme) {
		fprintf(stderr, "Failed to load cursor theme\n");
		return 1;
	}
	state.cursor_surface = wl_compositor_create_surface(state.compositor);

	struct escreen_output *output;
	wl_list_for_each(output, &state.outputs, link) {
		output->xdg_output = zxdg_output_manager_v1_get_xdg_output(state.xdg_output_manager, output->wl_output);
		zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
	}

	wl_display_roundtrip(state.display);

	if (delay > 0) {
		sleep(delay);
	}

	state.running = true;
	freeze_run(&state);
	selection_run(&state);

	if (state.result.width > 0 && state.result.height > 0) {
		crop_and_save(&state);
	}

	if (state.cursor_surface) wl_surface_destroy(state.cursor_surface);
	if (state.cursor_theme) wl_cursor_theme_destroy(state.cursor_theme);
	if (state.xkb_context) xkb_context_unref(state.xkb_context);
	if (state.global_capture) cairo_surface_destroy(state.global_capture);

	wl_display_disconnect(state.display);
	return 0;
}
