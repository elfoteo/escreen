#include <stdlib.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static point_t *current_points = NULL;
static size_t num_points = 0;
static size_t capacity = 0;

static void brush_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	num_points = 0;
	capacity = 16;
	current_points = (point_t*)malloc(capacity * sizeof(point_t));
	current_points[num_points++] = (point_t){x, y};
}

static void brush_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	if (num_points >= capacity) {
		capacity *= 2;
		current_points = (point_t*)realloc(current_points, capacity * sizeof(point_t));
	}
	current_points[num_points++] = (point_t){x, y};
}

static void brush_on_mouseup(struct escreen_state *state, double x, double y) {
	(void)x; (void)y;
	action_t action = {
		.type = TOOL_BRUSH,
		.r = state->sketching.r,
		.g = state->sketching.g,
		.b = state->sketching.b,
		.a = state->sketching.a,
		.thickness = state->sketching.thickness,
		.hardness = state->sketching.hardness,
		.points = current_points,
		.num_points = num_points
	};
	tools_add_action(state, action);
	current_points = NULL;
	num_points = 0;
}

static void draw_soft_spot(cairo_t *cr, double cx, double cy, double r, double g, double b, double a, double thickness, double hardness) {
	cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy, 0, cx, cy, thickness/2.0);
	cairo_pattern_add_color_stop_rgba(pat, 0, r, g, b, a);
	cairo_pattern_add_color_stop_rgba(pat, hardness, r, g, b, a);
	cairo_pattern_add_color_stop_rgba(pat, 1, r, g, b, 0);
	cairo_set_source(cr, pat);
	cairo_arc(cr, cx, cy, thickness/2.0, 0, 2*M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);
}

static void render_brush_internal(cairo_t *cr, point_t *points, size_t n, double r, double g, double b, double a, double thickness, double hardness) {
	if (n < 1) return;
	
	if (hardness >= 1.0) {
		cairo_set_source_rgba(cr, r, g, b, a);
		cairo_set_line_width(cr, thickness);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_move_to(cr, points[0].x, points[0].y);
		for (size_t i = 1; i < n; i++) {
			cairo_line_to(cr, points[i].x, points[i].y);
		}
		cairo_stroke(cr);
	} else {
		for (size_t i = 0; i < n; i++) {
			if (i > 0) {
				double dx = points[i].x - points[i-1].x;
				double dy = points[i].y - points[i-1].y;
				double dist = sqrt(dx*dx + dy*dy);
				double step = thickness * 0.1;
				if (step < 1.0) step = 1.0;
				for (double d = step; d < dist; d += step) {
					double cx = points[i-1].x + dx * (d / dist);
					double cy = points[i-1].y + dy * (d / dist);
					draw_soft_spot(cr, cx, cy, r, g, b, a, thickness, hardness);
				}
			}
			draw_soft_spot(cr, points[i].x, points[i].y, r, g, b, a, thickness, hardness);
		}
	}
}

static void brush_draw_preview(struct escreen_state *state, cairo_t *cr) {
	render_brush_internal(cr, current_points, num_points, 
		state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a,
		state->sketching.thickness, state->sketching.hardness);
}

static void brush_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	render_brush_internal(cr, action->points, action->num_points,
		action->r, action->g, action->b, action->a,
		action->thickness, action->hardness);
}

static void brush_on_draw_cursor_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a);
	cairo_set_line_width(cr, 1.0);
	cairo_arc(cr, x, y, state->sketching.thickness / 2.0, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_restore(cr);
}

tool_interface_t tool_brush = {
	.name = "Brush",
	.type = TOOL_BRUSH,
	.show_color = true,
	.show_thickness = true,
	.show_hardness = true,
	.show_fill = false,
	.on_mousedown = brush_on_mousedown,
	.on_mousemove = brush_on_mousemove,
	.on_mouseup = brush_on_mouseup,
	.draw_preview = brush_draw_preview,
	.render_action = brush_render_action,
	.on_draw_preview = brush_on_draw_cursor_preview,
};
