#include <stdlib.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static double sx, sy, ex, ey;

static void rect_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	sx = ex = x;
	sy = ey = y;
}

static void rect_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	ex = x;
	ey = y;
}

static void rect_on_mouseup(struct escreen_state *state, double x, double y) {
	ex = x;
	ey = y;
	action_t action = {};
	action.type = TOOL_RECTANGLE;
	action.r = state->sketching.r;
	action.g = state->sketching.g;
	action.b = state->sketching.b;
	action.a = state->sketching.a;
	action.thickness = state->sketching.thickness;
	action.filled = state->sketching.filled;
	action.x1 = sx; action.y1 = sy; action.x2 = ex; action.y2 = ey;

	tools_add_action(state, action);
}

static void render_rect_internal(cairo_t *cr, double x1, double y1, double x2, double y2, double r, double g, double b, double a, double thickness, bool filled) {
	double x = fmin(x1, x2);
	double y = fmin(y1, y2);
	double w = fabs(x2 - x1);
	double h = fabs(y2 - y1);
	
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_rectangle(cr, x, y, w, h);
	if (filled) {
		cairo_fill(cr);
	} else {
		cairo_set_line_width(cr, thickness);
		cairo_stroke(cr);
	}
}

static void rect_draw_preview(struct escreen_state *state, cairo_t *cr) {
	render_rect_internal(cr, sx, sy, ex, ey, 
		state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a,
		state->sketching.thickness, state->sketching.filled);
}

static void rect_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	render_rect_internal(cr, action->x1, action->y1, action->x2, action->y2,
		action->r, action->g, action->b, action->a,
		action->thickness, action->filled);
}

tool_interface_t tool_rectangle = {
	.name = "Rectangle",
	.type = TOOL_RECTANGLE,
	.show_color = true,
	.show_thickness = true,
	.show_hardness = false,
	.show_fill = true,
	.on_mousedown = rect_on_mousedown,
	.on_mousemove = rect_on_mousemove,
	.on_mouseup = rect_on_mouseup,
	.draw_preview = rect_draw_preview,
	.render_action = rect_render_action,
	.on_draw_preview = NULL,
};
