#include <stdlib.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static double sx, sy, ex, ey;

static void arrow_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	sx = ex = x;
	sy = ey = y;
}

static void arrow_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	ex = x;
	ey = y;
}

static void arrow_on_mouseup(struct escreen_state *state, double x, double y) {
	ex = x;
	ey = y;
	action_t action = {
		.type = TOOL_ARROW,
		.r = state->sketching.r,
		.g = state->sketching.g,
		.b = state->sketching.b,
		.a = state->sketching.a,
		.thickness = state->sketching.thickness,
		.x1 = sx, .y1 = sy, .x2 = ex, .y2 = ey
	};
	tools_add_action(state, action);
}

static void render_arrow_internal(cairo_t *cr, double x1, double y1, double x2, double y2, double r, double g, double b, double a, double thickness) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, thickness);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

	// Shaft
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);

	// Head
	double angle = atan2(y2 - y1, x2 - x1);
	double head_len = thickness * 3.0 + 8.0;
	if (head_len > 50) head_len = 50;

	cairo_move_to(cr, x2, y2);
	cairo_line_to(cr, x2 - head_len * cos(angle - M_PI/6.0), y2 - head_len * sin(angle - M_PI/6.0));
	cairo_stroke(cr);

	cairo_move_to(cr, x2, y2);
	cairo_line_to(cr, x2 - head_len * cos(angle + M_PI/6.0), y2 - head_len * sin(angle + M_PI/6.0));
	cairo_stroke(cr);
}

static void arrow_draw_preview(struct escreen_state *state, cairo_t *cr) {
	render_arrow_internal(cr, sx, sy, ex, ey, 
		state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a,
		state->sketching.thickness);
}

static void arrow_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	render_arrow_internal(cr, action->x1, action->y1, action->x2, action->y2,
		action->r, action->g, action->b, action->a,
		action->thickness);
}

static void arrow_on_draw_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a);
	cairo_set_line_width(cr, 1.0);
	cairo_arc(cr, x, y, state->sketching.thickness / 2.0, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_restore(cr);
}

tool_interface_t tool_arrow = {
	.name = "Arrow",
	.type = TOOL_ARROW,
	.show_color = true,
	.show_thickness = true,
	.show_hardness = false,
	.show_fill = false,
	.on_mousedown = arrow_on_mousedown,
	.on_mousemove = arrow_on_mousemove,
	.on_mouseup = arrow_on_mouseup,
	.draw_preview = arrow_draw_preview,
	.render_action = arrow_render_action,
	.on_draw_preview = arrow_on_draw_preview,
};
