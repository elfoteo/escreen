#include <stdlib.h>
#include "escreen.h"
#include "tools.h"

static double sx, sy, ex, ey;

static void line_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	sx = ex = x;
	sy = ey = y;
}

static void line_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	ex = x;
	ey = y;
}

static void line_on_mouseup(struct escreen_state *state, double x, double y) {
	ex = x;
	ey = y;
	action_t action = {
		.type = TOOL_LINE,
		.r = state->sketching.r,
		.g = state->sketching.g,
		.b = state->sketching.b,
		.a = state->sketching.a,
		.thickness = state->sketching.thickness,
		.x1 = sx, .y1 = sy, .x2 = ex, .y2 = ey
	};
	tools_add_action(state, action);
}

static void line_draw_preview(struct escreen_state *state, cairo_t *cr) {
	cairo_set_source_rgba(cr, state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a);
	cairo_set_line_width(cr, state->sketching.thickness);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, sx, sy);
	cairo_line_to(cr, ex, ey);
	cairo_stroke(cr);
}

static void line_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	cairo_set_source_rgba(cr, action->r, action->g, action->b, action->a);
	cairo_set_line_width(cr, action->thickness);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, action->x1, action->y1);
	cairo_line_to(cr, action->x2, action->y2);
	cairo_stroke(cr);
}

tool_interface_t tool_line = {
	.name = "Line",
	.type = TOOL_LINE,
	.on_mousedown = line_on_mousedown,
	.on_mousemove = line_on_mousemove,
	.on_mouseup = line_on_mouseup,
	.draw_preview = line_draw_preview,
	.render_action = line_render_action,
};
