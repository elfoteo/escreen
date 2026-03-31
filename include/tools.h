#ifndef TOOLS_H
#define TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <cairo.h>

struct escreen_state;

typedef enum {
	TOOL_SELECT,
	TOOL_LASSO,
	TOOL_BRUSH,
	TOOL_BLUR,
	TOOL_LINE,
	TOOL_RECTANGLE,
	TOOL_ARROW,
	TOOL_STAMP,
	TOOL_TEXT,
	TOOL_COUNT
} tool_type_t;

typedef struct {
	double x, y;
} point_t;

typedef struct {
	tool_type_t type;
	double r, g, b, a;
	double thickness;
	double hardness;
	bool filled;
	
	point_t *points;
	size_t num_points;
	
	// For line/rect
	double x1, y1, x2, y2;

	// For stamp
	int stamp_number;
	
	// For text
	char *text;
} action_t;

struct tool {
	const char *name;
	tool_type_t type;
	
	bool show_color;
	bool show_thickness;
	bool show_hardness;
	bool show_fill;
	
	void (*on_mousedown)(struct escreen_state *state, double x, double y);
	void (*on_mousemove)(struct escreen_state *state, double x, double y);
	void (*on_mouseup)(struct escreen_state *state, double x, double y);

	// Draw the currently active (in-progress) action
	void (*draw_preview)(struct escreen_state *state, cairo_t *cr);

	// Draw a completed action from the history
	void (*render_action)(struct escreen_state *state, cairo_t *cr, action_t *action);

	// Draw cursor preview at current pointer position
	void (*on_draw_preview)(struct escreen_state *state, cairo_t *cr, double x, double y);
};

typedef struct tool tool_interface_t;

// Tool Manager Functions
void tools_init(struct escreen_state *state);
void tools_cleanup(struct escreen_state *state);
void tools_update_history(struct escreen_state *state);
void tools_draw(struct escreen_state *state, cairo_t *cr);
void tools_draw_ui(struct escreen_state *state, cairo_t *cr);


void tools_set_active(struct escreen_state *state, tool_type_t type);
void tools_set_color(struct escreen_state *state, double r, double g, double b, double a);
void tools_set_thickness(struct escreen_state *state, double thickness);
void tools_set_hardness(struct escreen_state *state, double hardness);
void tools_set_filled(struct escreen_state *state, bool filled);

// Event dispatchers
void tools_handle_button(struct escreen_state *state, double x, double y, bool pressed);
void tools_handle_motion(struct escreen_state *state, double x, double y);
bool tools_is_on_toolbar(struct escreen_state *state, double x, double y);

void tools_add_action(struct escreen_state *state, action_t action);
void tools_handle_key(struct escreen_state *state, uint32_t sym, const char *utf8, bool shift_down, bool ctrl_down);

#ifdef __cplusplus
}
#endif

#endif
