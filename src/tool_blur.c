#include <stdlib.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static point_t *current_points = NULL;
static size_t num_points = 0;
static size_t capacity = 0;

static void blur_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	num_points = 0;
	capacity = 16;
	current_points = malloc(capacity * sizeof(point_t));
	current_points[num_points++] = (point_t){x, y};
}

static void blur_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	if (num_points >= capacity) {
		capacity *= 2;
		current_points = realloc(current_points, capacity * sizeof(point_t));
	}
	current_points[num_points++] = (point_t){x, y};
}

static void blur_on_mouseup(struct escreen_state *state, double x, double y) {
	(void)x; (void)y;
	action_t action = {
		.type = TOOL_BLUR,
		.thickness = state->sketching.thickness,
		.hardness = state->sketching.hardness,
		.points = current_points,
		.num_points = num_points
	};
	tools_add_action(state, action);
	current_points = NULL;
	num_points = 0;
}

static void render_blur_internal(struct escreen_state *state, cairo_t *cr, point_t *points, size_t n, double thickness, double amount) {
	if (n < 1 || !state->global_capture) return;
	
	cairo_save(cr);
	
	// Create a mask from the stroke
	cairo_push_group(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 1); // White for mask
	cairo_set_line_width(cr, thickness);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	
	if (n == 1) {
		cairo_arc(cr, points[0].x, points[0].y, thickness/2.0, 0, 2*M_PI);
		cairo_fill(cr);
	} else {
		cairo_move_to(cr, points[0].x, points[0].y);
		for (size_t i = 1; i < n; i++) {
			cairo_line_to(cr, points[i].x, points[i].y);
		}
		cairo_stroke(cr);
	}
	cairo_pattern_t *mask = cairo_pop_group(cr);
	
	// Blur by downscaling
	int32_t w = cairo_image_surface_get_width(state->global_capture);
	int32_t h = cairo_image_surface_get_height(state->global_capture);
	
	double scale = 1.0 / (1.0 + amount * 20.0); // Adjust amount sensitivity
	if (scale > 0.9) scale = 0.9;
	if (scale < 0.05) scale = 0.05;
	
	int sw = (int)(w * scale);
	int sh = (int)(h * scale);
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *scr = cairo_create(small);
	cairo_scale(scr, (double)sw/w, (double)sh/h);
	cairo_set_source_surface(scr, state->global_capture, 0, 0);
	cairo_paint(scr);
	cairo_destroy(scr);
	
	// Draw blurred back onto cr using mask
	cairo_pattern_t *blur_pattern = cairo_pattern_create_for_surface(small);
	cairo_matrix_t matrix;
	cairo_matrix_init_scale(&matrix, (double)sw/w, (double)sh/h);
	cairo_pattern_set_matrix(blur_pattern, &matrix);
	cairo_pattern_set_filter(blur_pattern, CAIRO_FILTER_BILINEAR);
	
	cairo_set_source(cr, blur_pattern);
	cairo_mask(cr, mask);
	
	cairo_pattern_destroy(blur_pattern);
	cairo_surface_destroy(small);
	cairo_pattern_destroy(mask);
	cairo_restore(cr);
}

static void blur_draw_preview(struct escreen_state *state, cairo_t *cr) {
	render_blur_internal(state, cr, current_points, num_points, state->sketching.thickness, state->sketching.hardness);
}

static void blur_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	render_blur_internal(state, cr, action->points, action->num_points, action->thickness, action->hardness);
}

tool_interface_t tool_blur = {
	.name = "Blur",
	.type = TOOL_BLUR,
	.on_mousedown = blur_on_mousedown,
	.on_mousemove = blur_on_mousemove,
	.on_mouseup = blur_on_mouseup,
	.draw_preview = blur_draw_preview,
	.render_action = blur_render_action,
};
