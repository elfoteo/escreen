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

	// Use floor: a pixel-center coordinate like 5.5 is on the boundary
	// between pixel 5 and 6, and floor correctly assigns it to pixel 5.
	const int px = (int)floor((lx - state->total_min_x) * state->max_scale_factor);
	const int py = (int)floor((ly - state->total_min_y) * state->max_scale_factor);

	if (px < 0 || py < 0 || px >= surf_w || py >= surf_h) return false;

	// Composite: global_capture + history_layer on top
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
	cairo_t *tcr = cairo_create(tmp);

	cairo_set_source_surface(tcr, state->global_capture, 0, 0);
	cairo_paint(tcr);

	if (state->sketching.history_layer) {
		cairo_set_source_surface(tcr, state->sketching.history_layer, 0, 0);
		cairo_paint_with_alpha(tcr, 1.0);
	}

	cairo_surface_flush(tmp);

	const unsigned char *data = cairo_image_surface_get_data(tmp);
	const int stride = cairo_image_surface_get_stride(tmp);

	const uint32_t pixel =
		*reinterpret_cast<const uint32_t *>(data + py * stride + px * 4);

	cairo_destroy(tcr);
	cairo_surface_destroy(tmp);

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

// Returns the logical coordinate of the center of the nearest physical pixel.
static void nearest_pixel_center(struct escreen_state *state,
                                  double lx, double ly,
                                  double *out_x, double *out_y)
{
	const int px = (int)((lx - state->total_min_x) * state->max_scale_factor + 0.5);
	const int py = (int)((ly - state->total_min_y) * state->max_scale_factor + 0.5);
	*out_x = (px + 0.5) / state->max_scale_factor + state->total_min_x;
	*out_y = (py + 0.5) / state->max_scale_factor + state->total_min_y;
}

// ---------------------------------------------------------------------------
// Tool callbacks
// ---------------------------------------------------------------------------

// Shared helper: sample at nearest pixel center and commit to active tool colour.
static void pick_at(struct escreen_state *state, double x, double y)
{
	double cx, cy;
	nearest_pixel_center(state, x, y, &cx, &cy);
	double r, g, b;
	if (sample_pixel(state, cx, cy, &r, &g, &b)) {
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
	// Snap to the nearest pixel center for consistent rendering
	double cx, cy;
	nearest_pixel_center(state, x, y, &cx, &cy);

	double r, g, b;
	const bool valid = sample_pixel(state, cx, cy, &r, &g, &b);
	if (!valid) { r = g = b = 0.5; }

	cairo_save(cr);

	double pop_anim = 0.0;
	uint64_t now = cp_get_time_ms();
	if (now - g_last_pick_ms < 150) {
		pop_anim = 1.0 - (double)(now - g_last_pick_ms) / 150.0;
	}
	double base_r = 78.0 + (pop_anim * 6.0);
	double outer_r = 86.0 + (pop_anim * 6.0);

	// Outer ring filled with the sampled color
	cairo_arc(cr, cx, cy, outer_r, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, r, g, b, 1.0);
	cairo_fill(cr);

	// 12x zoom lens: clip to inner circle, paint composited surface transformed
	if (state->global_capture && valid) {
		cairo_save(cr);

		cairo_arc(cr, cx, cy, base_r, 0, 2 * M_PI);
		cairo_clip(cr);

		// Build a composited surface: capture + history
		const int sw = cairo_image_surface_get_width(state->global_capture);
		const int sh = cairo_image_surface_get_height(state->global_capture);
		cairo_surface_t *comp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
		cairo_t *ccr = cairo_create(comp);
		cairo_set_source_surface(ccr, state->global_capture, 0, 0);
		cairo_paint(ccr);
		if (state->sketching.history_layer) {
			cairo_set_source_surface(ccr, state->sketching.history_layer, 0, 0);
			cairo_paint_with_alpha(ccr, 1.0);
		}
		cairo_destroy(ccr);

		cairo_pattern_t *pat = cairo_pattern_create_for_surface(comp);
		cairo_matrix_t mat;
		const double sc = state->max_scale_factor / 12.0;
		// cx,cy is already the pixel center from nearest_pixel_center().
		// Use floor (not round) to recover the pixel index — the center
		// coordinate (px+0.5) is on the rounding boundary and round would
		// give px+1.
		const int px = (int)floor((cx - state->total_min_x) * state->max_scale_factor);
		const int py = (int)floor((cy - state->total_min_y) * state->max_scale_factor);
		// Cairo pattern coords are edge-based; pixel (px,py) occupies [px,px+1]×[py,py+1]
		// so its center is at (px+0.5, py+0.5) in pattern space.
		// We want output point (cx,cy) to map to source point (px+0.5, py+0.5).
		// Mapping: source = (output - translate) / scale
		// => translate = output - scale * source
		const double tx = cx - sc * (px + 0.5);
		const double ty = cy - sc * (py + 0.5);
		cairo_matrix_init_identity(&mat);
		cairo_matrix_translate(&mat, tx, ty);
		cairo_matrix_scale(&mat, sc, sc);
		cairo_pattern_set_matrix(pat, &mat);
		cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);

		cairo_set_source(cr, pat);
		cairo_paint(cr);

		cairo_pattern_destroy(pat);
		cairo_surface_destroy(comp);
		cairo_restore(cr);
	}

	// Ring border
	cairo_set_line_width(cr, 2.0);
	cairo_arc(cr, cx, cy, base_r, 0, 2 * M_PI);
	const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
	if (luma > 0.55) cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
	else cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
	cairo_stroke(cr);

	// Crosshair centered on the pixel
	cairo_set_line_width(cr, 1.0);
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
	cairo_move_to(cr, cx - 5, cy); cairo_line_to(cr, cx + 5, cy);
	cairo_move_to(cr, cx, cy - 5); cairo_line_to(cr, cx, cy + 5);
	cairo_stroke(cr);

	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
	cairo_move_to(cr, cx - 4, cy); cairo_line_to(cr, cx + 4, cy);
	cairo_move_to(cr, cx, cy - 4); cairo_line_to(cr, cx, cy + 4);
	cairo_stroke(cr);

	// Hex label with rounded corners
	if (valid) {
		const int ri = (int)(r * 255.0 + 0.5);
		const int gi = (int)(g * 255.0 + 0.5);
		const int bi = (int)(b * 255.0 + 0.5);
		char hex[8];
		snprintf(hex, sizeof(hex), "#%02X%02X%02X", ri, gi, bi);

		const double font_size = 14.0;
		const double pad = 7.0;
		cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, font_size);

		cairo_text_extents_t te;
		cairo_text_extents(cr, hex, &te);

		const double LBL_W = te.width + pad * 2.0;
		const double LBL_H = font_size + pad * 2.0;
		const double lx = x - LBL_W * 0.5;
		const double ly = y + outer_r + 8.0;
		const double cr_r = 4.0;

		cairo_new_path(cr);
		cairo_arc(cr, lx + cr_r, ly + cr_r, cr_r, M_PI, 1.5 * M_PI);
		cairo_arc(cr, lx + LBL_W - cr_r, ly + cr_r, cr_r, 1.5 * M_PI, 2 * M_PI);
		cairo_arc(cr, lx + LBL_W - cr_r, ly + LBL_H - cr_r, cr_r, 0, 0.5 * M_PI);
		cairo_arc(cr, lx + cr_r, ly + LBL_H - cr_r, cr_r, 0.5 * M_PI, M_PI);
		cairo_close_path(cr);

		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.75);
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
	.shortcut       = 'C',
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
