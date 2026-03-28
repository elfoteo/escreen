#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "escreen.h"
#include "tools.h"

static double sx, sy;

static int current_stamp_counter = 1;

extern "C" void tool_stamp_reset_counter() {
	current_stamp_counter = 1;
}

// Exposed to tools.cpp
int* tool_stamp_get_counter_ptr() {
	return &current_stamp_counter;
}

static int get_next_stamp(struct escreen_state *state) {
	(void)state;
	return current_stamp_counter;
}

static void stamp_on_mousedown(struct escreen_state *state, double x, double y) {
	(void)state;
	sx = x;
	sy = y;
}

static void stamp_on_mousemove(struct escreen_state *state, double x, double y) {
	(void)state;
	sx = x;
	sy = y;
}

static void stamp_on_mouseup(struct escreen_state *state, double x, double y) {
	sx = x;
	sy = y;
	action_t action = {};
	action.type = TOOL_STAMP;
	action.r = state->sketching.r;
	action.g = state->sketching.g;
	action.b = state->sketching.b;
	action.a = state->sketching.a;
	action.thickness = state->sketching.thickness;
	action.x1 = sx; action.y1 = sy;
	action.stamp_number = current_stamp_counter++;

	tools_add_action(state, action);
}

static void draw_stamp(cairo_t *cr, double x, double y, double r, double g, double b, double a, double thickness, int number) {
	// Radius
	double radius = thickness > 12.0 ? thickness : 12.0; // Clamped so it's readable
	
	// Draw colored circle
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_arc(cr, x, y, radius, 0, 2 * M_PI);
	cairo_fill(cr);
	
	// Draw white rim/border to ensure it pops off the background
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_set_line_width(cr, 3.0);
	cairo_arc(cr, x, y, radius, 0, 2 * M_PI);
	cairo_stroke(cr);
	
	// Calculate luminance for text contrast
	double luminance = 0.299 * r + 0.587 * g + 0.114 * b;
	if (luminance > 0.5) {
		cairo_set_source_rgba(cr, 0, 0, 0, a); // Black text on bright backgrounds
	} else {
		cairo_set_source_rgba(cr, 1, 1, 1, a); // White text on dark backgrounds
	}

	char text[32];
	snprintf(text, sizeof(text), "%d", number);
	
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, radius * 1.5);
	
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);
	
	// Center text
	cairo_move_to(cr, x - extents.width / 2.0 - extents.x_bearing, y - extents.height / 2.0 - extents.y_bearing);
	cairo_show_text(cr, text);
}

static void stamp_draw_preview(struct escreen_state *state, cairo_t *cr) {
	int num = get_next_stamp(state);
	draw_stamp(cr, sx, sy, state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a, state->sketching.thickness, num);
}

static void stamp_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	draw_stamp(cr, action->x1, action->y1, action->r, action->g, action->b, action->a, action->thickness, action->stamp_number);
}

static void stamp_on_draw_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	int num = get_next_stamp(state);
	cairo_save(cr);
	draw_stamp(cr, x, y, state->sketching.r, state->sketching.g, state->sketching.b, 0.4 * state->sketching.a, state->sketching.thickness, num);
	cairo_restore(cr);
}

tool_interface_t tool_stamp = {
	.name = "Stamps",
	.type = TOOL_STAMP,
	.show_color = true,
	.show_thickness = true,
	.show_hardness = false,
	.show_fill = false,
	.on_mousedown = stamp_on_mousedown,
	.on_mousemove = stamp_on_mousemove,
	.on_mouseup = stamp_on_mouseup,
	.draw_preview = stamp_draw_preview,
	.render_action = stamp_render_action,
	.on_draw_preview = stamp_on_draw_preview,
};
