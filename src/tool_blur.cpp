#include <stdlib.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static point_t *current_points = NULL;
static size_t num_points = 0;
static size_t capacity = 0;
static cairo_surface_t *blur_mask_surface = NULL;
static cairo_surface_t *blurred_cache_surface = NULL;
static float last_blur_amount = -1.0f;

static void ensure_blur_cache(struct escreen_state *state, double amount) {
	if (blurred_cache_surface && last_blur_amount == (float)amount) return;
	
	if (blurred_cache_surface) cairo_surface_destroy(blurred_cache_surface);
	last_blur_amount = (float)amount;

	if (!state->global_capture) return;

	int w = cairo_image_surface_get_width(state->global_capture);
	int h = cairo_image_surface_get_height(state->global_capture);
	
	int step = (int)round(1.0 + amount * 19.0);
	if (step < 1) step = 1;
	if (step > 20) step = 20;
	double scale = 1.0 / step;

	int sw = (int)ceil(w * scale);
	int sh = (int)ceil(h * scale);
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	// Create downscaled version
	cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *scr = cairo_create(small);
	cairo_scale(scr, scale, scale);
	cairo_set_source_surface(scr, state->global_capture, 0, 0);
	cairo_paint(scr);
	if (state->sketching.history_layer) {
		cairo_set_source_surface(scr, state->sketching.history_layer, 0, 0);
		cairo_paint(scr);
	}
	cairo_destroy(scr);

	// Create upscaled blurred cache
	blurred_cache_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *bcr = cairo_create(blurred_cache_surface);
	cairo_scale(bcr, (double)step, (double)step);
	cairo_set_source_surface(bcr, small, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_BILINEAR);
	cairo_paint(bcr);
	cairo_destroy(bcr);
	cairo_surface_destroy(small);
}

static void blur_on_mousedown(struct escreen_state *state, double x, double y) {
	num_points = 0;
	capacity = 16;
	current_points = (point_t*)malloc(capacity * sizeof(point_t));
	current_points[num_points++] = (point_t){x, y};

	ensure_blur_cache(state, state->sketching.hardness);

	if (!blur_mask_surface && state->global_capture) {
		int w = cairo_image_surface_get_width(state->global_capture);
		int h = cairo_image_surface_get_height(state->global_capture);
		blur_mask_surface = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
	}

	if (blur_mask_surface) {
		cairo_t *cr = cairo_create(blur_mask_surface);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_arc(cr, x - state->total_min_x, y - state->total_min_y, state->sketching.thickness / 2.0, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_destroy(cr);
	}
}

static void blur_on_mousemove(struct escreen_state *state, double x, double y) {
	if (num_points >= capacity) {
		capacity *= 2;
		current_points = (point_t*)realloc(current_points, capacity * sizeof(point_t));
	}
	current_points[num_points++] = (point_t){x, y};

	if (blur_mask_surface && num_points >= 2) {
		cairo_t *cr = cairo_create(blur_mask_surface);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_set_line_width(cr, state->sketching.thickness);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_move_to(cr, current_points[num_points-2].x - state->total_min_x, current_points[num_points-2].y - state->total_min_y);
		cairo_line_to(cr, x - state->total_min_x, y - state->total_min_y);
		cairo_stroke(cr);
		cairo_destroy(cr);
	}
}

static void blur_on_mouseup(struct escreen_state *state, double x, double y) {
	(void)x; (void)y;
	action_t action = {};
	action.type = TOOL_BLUR;
	action.thickness = state->sketching.thickness;
	action.hardness = state->sketching.hardness;
	action.points = current_points;
	action.num_points = num_points;

	tools_add_action(state, action);
	current_points = NULL;
	num_points = 0;
}

static void render_blur_internal(struct escreen_state *state, cairo_t *cr, point_t *points, size_t n, double thickness, double amount) {
	if (n < 1 || !state->global_capture) return;

	// Compute bounding box of the stroke (in logical/global coordinates)
	double bx0 = points[0].x, by0 = points[0].y;
	double bx1 = points[0].x, by1 = points[0].y;
	for (size_t i = 1; i < n; i++) {
		if (points[i].x < bx0) bx0 = points[i].x;
		if (points[i].y < by0) by0 = points[i].y;
		if (points[i].x > bx1) bx1 = points[i].x;
		if (points[i].y > by1) by1 = points[i].y;
	}
	double pad = thickness / 2.0 + 2.0;
	bx0 -= pad; by0 -= pad;
	bx1 += pad; by1 += pad;

	// Determine scale factor for max-output scale (tiles may be HiDPI)
	int max_scale = 1;
	struct escreen_output *o;
	wl_list_for_each(o, &state->outputs, link) {
		if (o->scale > max_scale) max_scale = o->scale;
	}

	// Convert logical coords to physical pixel coords in global_capture
	// global_capture is stitched at max_scale, origin = (total_min_x, total_min_y)
	int cx0 = (int)((bx0 - state->total_min_x) * max_scale);
	int cy0 = (int)((by0 - state->total_min_y) * max_scale);
	int cx1 = (int)((bx1 - state->total_min_x) * max_scale) + 1;
	int cy1 = (int)((by1 - state->total_min_y) * max_scale) + 1;

	// Blur scale decides the downscale factor.
	// Force it to be an exact fraction (1/step) to lock the sampling grid perfectly.
	int step = (int)round(1.0 + amount * 19.0);
	if (step < 1) step = 1;
	if (step > 20) step = 20;
	double scale = 1.0 / step;

	// Snap the extraction box to the grid to prevent resampling jitter
	int scx0 = (int)floor((double)cx0 / step) * step;
	int scy0 = (int)floor((double)cy0 / step) * step;
	int scx1 = (int)ceil((double)cx1 / step) * step;
	int scy1 = (int)ceil((double)cy1 / step) * step;

	int rw = scx1 - scx0;
	int rh = scy1 - scy0;
	if (rw <= 0 || rh <= 0) return;

	int sw = rw / step;
	int sh = rh / step;
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	// Extract just the aligned bounding box region
	cairo_surface_t *region = cairo_image_surface_create(CAIRO_FORMAT_RGB24, rw, rh);
	cairo_t *rcr = cairo_create(region);
	cairo_set_source_surface(rcr, state->global_capture, -scx0, -scy0);
	cairo_paint(rcr);
	if (state->sketching.history_layer) {
		cairo_set_source_surface(rcr, state->sketching.history_layer, -scx0, -scy0);
		cairo_paint(rcr);
	}
	cairo_destroy(rcr);

	// Downscale using the exact scale
	cairo_surface_t *small_region = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *scr = cairo_create(small_region);
	cairo_scale(scr, scale, scale);
	cairo_set_source_surface(scr, region, 0, 0);
	cairo_paint(scr);
	cairo_destroy(scr);
	cairo_surface_destroy(region);

	// Build blurred pattern mapping exactly from physical grid
	cairo_pattern_t *blur_pattern = cairo_pattern_create_for_surface(small_region);
	cairo_matrix_t matrix;
	cairo_matrix_init_identity(&matrix);
	cairo_matrix_scale(&matrix, max_scale * scale, max_scale * scale);
	cairo_matrix_translate(&matrix, -state->total_min_x - (double)scx0 / max_scale, -state->total_min_y - (double)scy0 / max_scale);
	cairo_pattern_set_matrix(blur_pattern, &matrix);
	cairo_pattern_set_filter(blur_pattern, CAIRO_FILTER_BILINEAR);

	cairo_save(cr);

	// Create stroke mask in logical space
	cairo_push_group(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_set_line_width(cr, thickness);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	if (n == 1) {
		cairo_arc(cr, points[0].x, points[0].y, thickness / 2.0, 0, 2 * M_PI);
		cairo_fill(cr);
	} else {
		cairo_move_to(cr, points[0].x, points[0].y);
		for (size_t i = 1; i < n; i++) {
			cairo_line_to(cr, points[i].x, points[i].y);
		}
		cairo_stroke(cr);
	}
	cairo_pattern_t *mask = cairo_pop_group(cr);

	cairo_set_source(cr, blur_pattern);
	cairo_mask(cr, mask);

	cairo_pattern_destroy(blur_pattern);
	cairo_surface_destroy(small_region);
	cairo_pattern_destroy(mask);
	cairo_restore(cr);
}

static void blur_draw_preview(struct escreen_state *state, cairo_t *cr) {
	if (blurred_cache_surface && blur_mask_surface) {
		cairo_save(cr);
		cairo_set_source_surface(cr, blurred_cache_surface, state->total_min_x, state->total_min_y);
		cairo_mask_surface(cr, blur_mask_surface, state->total_min_x, state->total_min_y);
		cairo_restore(cr);
	}
}

static void blur_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	render_blur_internal(state, cr, action->points, action->num_points, action->thickness, action->hardness);
}

static void blur_on_draw_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.5);
	cairo_set_line_width(cr, 1.0);
	cairo_arc(cr, x, y, state->sketching.thickness / 2.0, 0, 6.28318530718);
	cairo_stroke(cr);
	cairo_restore(cr);
}

tool_interface_t tool_blur = {
	.name = "Blur",
	.type = TOOL_BLUR,
	.show_color = false,
	.show_thickness = true,
	.show_hardness = true,
	.show_fill = false,
	.on_mousedown = blur_on_mousedown,
	.on_mousemove = blur_on_mousemove,
	.on_mouseup = blur_on_mouseup,
	.draw_preview = blur_draw_preview,
	.render_action = blur_render_action,
	.on_draw_preview = blur_on_draw_preview,
};
