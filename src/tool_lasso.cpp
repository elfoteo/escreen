#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include "escreen.h"
#include "tools.h"

static point_t *current_points = NULL;
static size_t num_points = 0;
static size_t capacity = 0;

extern "C" {

static void lasso_on_mousedown(struct escreen_state *state, double x, double y) {
	num_points = 0;
	capacity = 64;
	current_points = (point_t*)malloc(capacity * sizeof(point_t));
	current_points[num_points++] = (point_t){x, y};
    (void)state;
}

static void lasso_on_mousemove(struct escreen_state *state, double x, double y) {
	if (num_points > 0) {
        double last_x = current_points[num_points-1].x;
        double last_y = current_points[num_points-1].y;
        double dx = x - last_x;
        double dy = y - last_y;
        if (dx*dx + dy*dy < 4.0) return; // Only add if moved at least 2 pixels
    }

    if (num_points >= capacity) {
		capacity *= 2;
		current_points = (point_t*)realloc(current_points, capacity * sizeof(point_t));
	}
	current_points[num_points++] = (point_t){x, y};
    (void)state;
}

static void lasso_on_mouseup(struct escreen_state *state, double x, double y) {
	if (num_points < 3) {
        if (current_points) free(current_points);
        current_points = NULL;
        num_points = 0;
        return;
    }

    if (state->sketching.lasso_points) free(state->sketching.lasso_points);
    state->sketching.lasso_points = current_points;
    state->sketching.lasso_num_points = num_points;

    double min_x = state->sketching.lasso_points[0].x;
    double max_x = state->sketching.lasso_points[0].x;
    double min_y = state->sketching.lasso_points[0].y;
    double max_y = state->sketching.lasso_points[0].y;

    for (size_t i = 1; i < state->sketching.lasso_num_points; i++) {
        if (state->sketching.lasso_points[i].x < min_x) min_x = state->sketching.lasso_points[i].x;
        if (state->sketching.lasso_points[i].x > max_x) max_x = state->sketching.lasso_points[i].x;
        if (state->sketching.lasso_points[i].y < min_y) min_y = state->sketching.lasso_points[i].y;
        if (state->sketching.lasso_points[i].y > max_y) max_y = state->sketching.lasso_points[i].y;
    }

    state->result.x = (int32_t)min_x;
    state->result.y = (int32_t)min_y;
    state->result.width = (int32_t)(max_x - min_x);
    state->result.height = (int32_t)(max_y - min_y);
    state->sketching.history_rendered_count = 0;

    current_points = NULL;
    num_points = 0;
}

static void lasso_draw_preview(struct escreen_state *state, cairo_t *cr) {
	if (num_points < 2) return;

    cairo_save(cr);
    cairo_set_source_rgba(cr, state->config.colors.accent.r, state->config.colors.accent.g, state->config.colors.accent.b, 0.9);
    cairo_set_line_width(cr, 2.0);
    
    // Dashed line for preview - slightly animated dash might be cool but for now static
    double dashes[] = {5.0, 5.0};
    cairo_set_dash(cr, dashes, 2, 0);

    cairo_move_to(cr, current_points[0].x, current_points[0].y);
    for (size_t i = 1; i < num_points; i++) {
        cairo_line_to(cr, current_points[i].x, current_points[i].y);
    }
    cairo_line_to(cr, current_points[0].x, current_points[0].y); 
    cairo_stroke(cr);

    // Subtle fill to show the area inside
    cairo_set_source_rgba(cr, state->config.colors.accent.r, state->config.colors.accent.g, state->config.colors.accent.b, 0.15);
    cairo_move_to(cr, current_points[0].x, current_points[0].y);
    for (size_t i = 1; i < num_points; i++) {
        cairo_line_to(cr, current_points[i].x, current_points[i].y);
    }
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void lasso_on_draw_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, state->config.colors.accent.r, state->config.colors.accent.g, state->config.colors.accent.b, 0.7);
	cairo_set_line_width(cr, 1.2);
	
    // Draw a small lasso icon at cursor (dotted loop)
    cairo_translate(cr, x, y);
    double r = 8.0;
    
    // Draw dotted loop manually
    for (double a = 0; a < 2 * M_PI; a += 0.4) {
        cairo_arc(cr, cos(a) * r, sin(a) * r, 1.0, 0, 2*M_PI);
        cairo_fill(cr);
    }
    
    // Add "end" of the rope
    cairo_move_to(cr, r, 0);
    cairo_line_to(cr, r + 4, 4);
	cairo_stroke(cr);
	cairo_restore(cr);
    (void)state;
}

tool_interface_t tool_lasso = {
	.name = "Lasso Select",
	.type = TOOL_LASSO,
	.show_color = false,
	.show_thickness = false,
	.show_hardness = false,
	.show_fill = false,
	.on_mousedown = lasso_on_mousedown,
	.on_mousemove = lasso_on_mousemove,
	.on_mouseup = lasso_on_mouseup,
	.draw_preview = lasso_draw_preview,
	.render_action = NULL, // No permanent action for lasso
	.on_draw_preview = lasso_on_draw_preview,
};

} // extern "C"
