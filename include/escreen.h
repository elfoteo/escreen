#ifndef ESCREEN_H
#define ESCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo.h>
#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
#ifdef __cplusplus
#define namespace _namespace
#endif

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#ifdef __cplusplus
#undef namespace
#endif
#include "tools.h"
#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
	SELECTION_NOT_STARTED,
	SELECTION_DRAGGING,
	SELECTION_EDITING,
	SELECTION_MOVING,
	SELECTION_RESIZING,
} selection_status_t;

typedef enum {
	HANDLE_NONE,
	HANDLE_TOP_LEFT,
	HANDLE_TOP_RIGHT,
	HANDLE_BOTTOM_LEFT,
	HANDLE_BOTTOM_RIGHT,
	HANDLE_TOP,
	HANDLE_BOTTOM,
	HANDLE_LEFT,
	HANDLE_RIGHT,
} handle_type_t;

struct escreen_box {
	int32_t x, y, width, height;
};

struct pool_buffer {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int32_t width, height, stride;
	bool busy;
	struct escreen_output *owner;
	pixman_image_t *pixman;
	cairo_surface_t *cairo_surface; // Still used for border/handles for now if we want to mix
	cairo_t *cairo;
};

struct escreen_state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_compositor *compositor;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_screencopy_manager_v1 *screencopy_manager;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct zwlr_data_control_manager_v1 *data_control_manager;

	struct wl_list outputs; // struct escreen_output::link
	struct wl_list seats;   // struct escreen_seat::link

	bool running;
	struct escreen_box result;
	int32_t total_min_x, total_min_y;
	int32_t total_max_x, total_max_y;

	bool clipboard;
	bool save_file;

	struct wl_cursor_theme *cursor_theme;
	struct wl_surface *cursor_surface;

	struct xkb_context *xkb_context;
	cairo_surface_t *global_capture;

	// Tools
	struct {
		tool_interface_t *tools[TOOL_COUNT];
		tool_interface_t *active_tool;
		
		double r, g, b, a;
		double thickness;
		double hardness;
		bool filled;
		
		action_t *history;
		size_t history_count;
		size_t history_undo_pos;
		size_t history_capacity;
		
		cairo_surface_t *history_layer;
		size_t history_rendered_count;
		
		bool drawing;
	} sketching;
};

#ifdef __cplusplus
}
#endif

struct escreen_output {
	struct wl_output *wl_output;
	struct escreen_state *state;
	struct wl_list link;

	struct escreen_box geometry;
	struct escreen_box logical_geometry;
	int32_t scale;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct zxdg_output_v1 *xdg_output;

	int32_t width, height;
	struct pool_buffer *current_buffer;
	struct pool_buffer buffers[3];
	
	struct pool_buffer frozen_buffer;
	bool frozen_captured;
	bool frozen_failed;
	pixman_image_t *darkened_image; // pre-baked: frozen + 40% dark, physical resolution
	
	bool configured;
	bool dirty;
	struct wl_callback *frame_callback;
};

struct escreen_seat {
	struct escreen_state *state;
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;
	struct wl_keyboard *wl_keyboard;
	struct wl_list link;

	struct escreen_output *current_output;
	int32_t x, y; // Global coordinates
	int32_t anchor_x, anchor_y;
	uint32_t pointer_serial;
	bool button_pressed;
	bool has_selection;
	
	selection_status_t selection_status;
	handle_type_t active_handle;

	struct {
		struct xkb_keymap *keymap;
		struct xkb_state *state;
	} xkb;
};

// Functions
void freeze_run(struct escreen_state *state);
void crop_and_save(struct escreen_state *state);
void selection_run(struct escreen_state *state);
void image_save(struct escreen_state *state, void *data, int32_t width, int32_t height, int32_t stride);

// Clipboard
void clipboard_send_data(struct escreen_state *state, void *data, size_t size);

#endif
