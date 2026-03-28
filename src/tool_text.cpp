#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "escreen.h"
#include "tools.h"

static void text_on_mousedown(struct escreen_state *state, double x, double y) {
	if (!state->sketching.is_text_editing) {
		state->sketching.is_text_editing = true;
		state->sketching.text_buffer[0] = '\0';
		state->sketching.text_cursor_pos = 0;
	}
	state->sketching.text_x = x;
	state->sketching.text_y = y;
}

static void text_on_mousemove(struct escreen_state *state, double x, double y) {
	if (state->sketching.is_text_editing && state->sketching.drawing) {
		state->sketching.text_x = x;
		state->sketching.text_y = y;
	}
}

static void draw_text_multiline(cairo_t *cr, double x, double y, const char *text, double font_size) {
	if (!text) return;
	
	char *copy = strdup(text);
	char *line = copy;
	double current_y = y;
	double line_spacing = font_size * 1.2;

	while (line) {
		char *next_line = strchr(line, '\n');
		if (next_line) *next_line = '\0';
		
		cairo_move_to(cr, x, current_y);
		cairo_show_text(cr, line);
		
		if (next_line) {
			line = next_line + 1;
			current_y += line_spacing;
		} else {
			line = NULL;
		}
	}
	free(copy);
}

static void text_render_action(struct escreen_state *state, cairo_t *cr, action_t *action) {
	(void)state;
	if (!action->text) return;

	cairo_save(cr);
	cairo_set_source_rgba(cr, action->r, action->g, action->b, action->a);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	double font_size = action->thickness * 2.0 + 10.0;
	cairo_set_font_size(cr, font_size);
	
	draw_text_multiline(cr, action->x1, action->y1, action->text, font_size);
	cairo_restore(cr);
}

static void text_draw_preview(struct escreen_state *state, cairo_t *cr) {
	if (!state->sketching.is_text_editing) return;

	cairo_save(cr);
	cairo_set_source_rgba(cr, state->sketching.r, state->sketching.g, state->sketching.b, state->sketching.a);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	double font_size = state->sketching.thickness * 2.0 + 10.0;
	cairo_set_font_size(cr, font_size);
	
	draw_text_multiline(cr, state->sketching.text_x, state->sketching.text_y, state->sketching.text_buffer, font_size);
	
	// Cursor positioning
	int cursor_pos = state->sketching.text_cursor_pos;
	const char *buf = state->sketching.text_buffer;
	
	int line_count = 0;
	const char *line_start = buf;
	for (int i = 0; i < cursor_pos; i++) {
		if (buf[i] == '\n') {
			line_count++;
			line_start = buf + i + 1;
		}
	}
	
	// Measure text on current line up to cursor
	char current_line_fragment[256];
	int fragment_len = (buf + cursor_pos) - line_start;
	if (fragment_len >= (int)sizeof(current_line_fragment)) fragment_len = sizeof(current_line_fragment) - 1;
	memcpy(current_line_fragment, line_start, fragment_len);
	current_line_fragment[fragment_len] = '\0';
	
	cairo_text_extents_t extents;
	cairo_text_extents(cr, current_line_fragment, &extents);
	
	double line_spacing = font_size * 1.2;
	double cursor_x = state->sketching.text_x + extents.x_advance + 2.0;
	double cursor_y = state->sketching.text_y + line_count * line_spacing;

	// Blinking logic with reset on last_key_time
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	uint32_t delta = now_ms - state->sketching.last_key_time;
	
	// Stay solid for 500ms after keypress, then blink
	bool blink = (delta < 500) || ((delta / 500) % 2 == 0);

	if (blink) {
		cairo_set_line_width(cr, 2.0);
		cairo_move_to(cr, cursor_x, cursor_y - font_size * 0.8);
		cairo_line_to(cr, cursor_x, cursor_y + font_size * 0.2);
		cairo_stroke(cr);
	}
	
	cairo_restore(cr);
}

static void text_on_draw_preview(struct escreen_state *state, cairo_t *cr, double x, double y) {
	if (state->sketching.is_text_editing) return;

	cairo_save(cr);
	cairo_set_source_rgba(cr, state->sketching.r, state->sketching.g, state->sketching.b, 0.4);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, state->sketching.thickness * 2.0 + 10.0);
	
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, "Type...");
	cairo_restore(cr);
}

tool_interface_t tool_text = {
	.name = "Text",
	.type = TOOL_TEXT,
	.show_color = true,
	.show_thickness = true,
	.show_hardness = false,
	.show_fill = false,
	.on_mousedown = text_on_mousedown,
	.on_mousemove = text_on_mousemove,
	.on_mouseup = NULL,
	.draw_preview = text_draw_preview,
	.render_action = text_render_action,
	.on_draw_preview = text_on_draw_preview,
};
