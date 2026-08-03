#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "escreen.h"
#include "tools.h"

static uint64_t cp_get_time_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t g_last_pick_ms = 0;

// ---------------------------------------------------------------------------
// Pixel sampling
// ---------------------------------------------------------------------------

// Sample the premultiplied ARGB32 cairo surface at a logical (screen-space)
// coordinate.  Returns false when (x,y) is outside the capture surface.
// Output channels are in [0, 1].
static bool sample_pixel(struct escreen_state *state,
                          double lx, double ly,
                          double *out_r, double *out_g, double *out_b)
{
	if (!state->global_capture) return false;

	cairo_surface_flush(state->global_capture);

	const int surf_w = cairo_image_surface_get_width(state->global_capture);
	const int surf_h = cairo_image_surface_get_height(state->global_capture);

	// Logical → physical pixel coordinates inside global_capture
	const int px = (int)((lx - state->total_min_x) * state->max_scale_factor);
	const int py = (int)((ly - state->total_min_y) * state->max_scale_factor);

	if (px < 0 || py < 0 || px >= surf_w || py >= surf_h) return false;

	const unsigned char *data = cairo_image_surface_get_data(state->global_capture);
	if (!data) return false;

	const int stride = cairo_image_surface_get_stride(state->global_capture);

	// Cairo ARGB32 (native-endian): byte layout on little-endian is B G R A
	const uint32_t pixel =
		*reinterpret_cast<const uint32_t *>(data + py * stride + px * 4);

	// Un-premultiply: cairo stores premultiplied values
	const uint8_t a_byte = (pixel >> 24) & 0xFF;
	const uint8_t r_byte = (pixel >> 16) & 0xFF;
	const uint8_t g_byte = (pixel >>  8) & 0xFF;
	const uint8_t b_byte = (pixel >>  0) & 0xFF;

	if (a_byte == 0) {
		*out_r = *out_g = *out_b = 0.0;
	} else {
		*out_r = r_byte / (double)a_byte;
		*out_g = g_byte / (double)a_byte;
		*out_b = b_byte / (double)a_byte;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Tool callbacks
// ---------------------------------------------------------------------------

// Shared helper: sample at (x, y) and commit to active tool colour.
static void pick_at(struct escreen_state *state, double x, double y)
{
	double r, g, b;
	if (sample_pixel(state, x, y, &r, &g, &b)) {
		state->sketching.r = r;
		state->sketching.g = g;
		state->sketching.b = b;
	}
}

static void colorpicker_on_mousedown(struct escreen_state *state, double x, double y)
{
	pick_at(state, x, y);
}

static void colorpicker_on_mousemove(struct escreen_state *state, double x, double y)
{
	// Update the active colour live while the mouse button is held so the
	// swatch in the toolbar reflects the pixel under the cursor.
	pick_at(state, x, y);
}

static void colorpicker_on_mouseup(struct escreen_state *state, double x, double y)
{
	// Final, committed pick.
	pick_at(state, x, y);
	g_last_pick_ms = cp_get_time_ms();
	state->sketching.ui_layout_frames = 20; // Ensure continuous redraws for animation
}

// ---------------------------------------------------------------------------
// Cursor preview
// ---------------------------------------------------------------------------

// Draws the eyedropper cursor preview (swatch + hex label) near the cursor.
// Called from selection.c every frame while this tool is active, regardless
// of whether the mouse button is held.
static void colorpicker_on_draw_preview(struct escreen_state *state,
                                         cairo_t *cr, double x, double y)
{
	// Sample the pixel under the current cursor position for the live preview.
	double r, g, b;
	const bool valid = sample_pixel(state, x, y, &r, &g, &b);
	if (!valid) { r = g = b = 0.5; }

	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

    // Animation scale
	double pop_anim = 0.0;
	uint64_t now = cp_get_time_ms();
	if (now - g_last_pick_ms < 150) {
		pop_anim = 1.0 - (double)(now - g_last_pick_ms) / 150.0;
	}
	double base_r = 16.0 + (pop_anim * 6.0);

    // Magnifier ring with sampled color
	cairo_set_line_width(cr, 6.0);
	cairo_arc(cr, x, y, base_r, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, r, g, b, 1.0);
	cairo_stroke_preserve(cr);
    
    // Outer contrast border
	cairo_set_line_width(cr, 1.0);
	const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
	if (luma > 0.55) cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
	else cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8 - (pop_anim * 0.8));
	cairo_stroke(cr);
    
    // Inner contrast border
	cairo_arc(cr, x, y, base_r - 3.5, 0, 2 * M_PI);
	cairo_stroke(cr);

    // Fine crosshair in the center
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_move_to(cr, x - 5, y); cairo_line_to(cr, x + 5, y);
    cairo_move_to(cr, x, y - 5); cairo_line_to(cr, x, y + 5);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
    cairo_move_to(cr, x - 4, y); cairo_line_to(cr, x + 4, y);
    cairo_move_to(cr, x, y - 4); cairo_line_to(cr, x, y + 4);
    cairo_stroke(cr);

	// Hex label
	if (valid) {
		const int ri = (int)(r * 255.0 + 0.5);
		const int gi = (int)(g * 255.0 + 0.5);
		const int bi = (int)(b * 255.0 + 0.5);
		char hex[8];
		snprintf(hex, sizeof(hex), "#%02X%02X%02X", ri, gi, bi);

		const double font_size = 13.0;
		const double pad = 6.0;
		cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, font_size);

		cairo_text_extents_t te;
		cairo_text_extents(cr, hex, &te);

		const double LBL_W = te.width + pad * 2.0;
		const double LBL_H = font_size + pad * 2.0;
		const double lx = x - LBL_W * 0.5;
		const double ly = y + 17.0;

		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.68);
		cairo_rectangle(cr, lx, ly, LBL_W, LBL_H);
		cairo_fill(cr);

		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
		cairo_move_to(cr,
			lx + (LBL_W - te.width) * 0.5 - te.x_bearing,
			ly + (LBL_H - te.height) * 0.5 - te.y_bearing);
		cairo_show_text(cr, hex);
	}

	cairo_restore(cr);
}

// ---------------------------------------------------------------------------
// Tool descriptor
// ---------------------------------------------------------------------------

tool_interface_t tool_colorpicker = {
	.name           = "Color Picker",
	.type           = TOOL_COLORPICKER,
	.show_color     = true,
	.show_thickness = false,
	.show_hardness  = false,
	.show_fill      = false,
	.on_mousedown   = colorpicker_on_mousedown,
	.on_mousemove   = colorpicker_on_mousemove,
	.on_mouseup     = colorpicker_on_mouseup,
	.draw_preview   = NULL,   // no in-progress canvas stroke
	.render_action  = NULL,   // produces no History action
	.on_draw_preview = colorpicker_on_draw_preview,
};
