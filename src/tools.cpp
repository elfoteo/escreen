#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "escreen.h"
#include "tools.h"

extern "C" tool_interface_t tool_brush;
extern "C" tool_interface_t tool_blur;
extern "C" tool_interface_t tool_line;
extern "C" tool_interface_t tool_rectangle;
extern "C" tool_interface_t tool_arrow;
extern "C" tool_interface_t tool_stamp;
extern "C" tool_interface_t tool_text;
extern "C" tool_interface_t tool_lasso;
extern "C" tool_interface_t tool_colorpicker;

int* tool_stamp_get_counter_ptr();

tool_interface_t tool_select = {
	"Select Area",
	TOOL_SELECT,
	0,                      // shortcut
	false, false, false, false, // UI flags: color, thickness, hardness, fill
	NULL, NULL, NULL, NULL, NULL, NULL // Callbacks including on_draw_preview
};

// ---- small helpers ---------------------------------------------------------

static uint64_t ui_get_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static void rgb_to_hsv(double r, double g, double b, double *h, double *s, double *v) {
	double max = fmax(r, fmax(g, b)), min = fmin(r, fmin(g, b));
	double d = max - min;
	*v = max;
	*s = (max == 0.0) ? 0.0 : d / max;
	if (d == 0.0) { *h = 0.0; return; }
	double hh;
	if (max == r) hh = fmod((g - b) / d, 6.0);
	else if (max == g) hh = (b - r) / d + 2.0;
	else hh = (r - g) / d + 4.0;
	hh *= 60.0;
	if (hh < 0.0) hh += 360.0;
	*h = hh / 360.0;
}

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b) {
	if (s <= 0.0) { *r = *g = *b = v; return; }
	h = fmod(h, 1.0); if (h < 0.0) h += 1.0;
	int i = (int)(h * 6.0);
	double f = h * 6.0 - i;
	double p = v * (1.0 - s);
	double q = v * (1.0 - s * f);
	double t = v * (1.0 - s * (1.0 - f));
	switch (i % 6) {
		case 0: *r=v; *g=t; *b=p; break;
		case 1: *r=q; *g=v; *b=p; break;
		case 2: *r=p; *g=v; *b=t; break;
		case 3: *r=p; *g=q; *b=v; break;
		case 4: *r=t; *g=p; *b=v; break;
		default: *r=v; *g=p; *b=q; break;
	}
}

// ---- widget rect / layout ---------------------------------------------------

typedef struct { double x, y, w, h; } ui_rect_t;

typedef struct {
	double x, y, w, h;
	ui_rect_t handle;
	ui_rect_t icons[TOOL_COUNT];
	ui_rect_t wheel, hue;           // SV square + hue strip of the color picker
	ui_rect_t sl_thick, sl_hard;    // sliders
	ui_rect_t cb_fill;              // fill checkbox
	ui_rect_t stamp_minus, stamp_num, stamp_plus;
	ui_rect_t shortcuts[TOOL_COUNT];  // shortcut key boxes per tool type
} ui_layout_t;

// Widget ids, shared between drawing and hit-testing.
enum {
	UI_NONE = 0,
	UI_HANDLE,
	UI_ICON_BASE,        // + tool_type  (icons occupy UI_ICON_BASE..UI_ICON_BASE+TOOL_COUNT-1)
	UI_WHEEL = UI_ICON_BASE + (int)TOOL_COUNT,
	UI_HUE,
	UI_SLIDER_THICK,
	UI_SLIDER_HARD,
	UI_CHECKBOX_FILL,
	UI_STAMP_MINUS,
	UI_STAMP_PLUS,
	UI_SHORTCUT_BASE,    // + shortcut index (0..7)
};
#define UI_ICON(type) (UI_ICON_BASE + (int)(type))

static const double UI_PAD         = 8.0;
static const double UI_GRIP_STEP   = 4.0;
static const double UI_GRIP_R      = 1.4;
static const double UI_HANDLE_PAD  = 10.0;  // padding around the grip graphics
static const double UI_HANDLE_MARG = 4.0;   // vertical margin around the drag button
static const double UI_GRIP_W      = 4.0 * UI_GRIP_STEP + 2.0 * UI_GRIP_R; // 5 grip columns
static const double UI_GRIP_H      = 2.0 * UI_GRIP_STEP + 2.0 * UI_GRIP_R; // 3 grip rows
static const double UI_HANDLE_W    = UI_GRIP_W + 2.0 * UI_HANDLE_PAD;      // drag button width
static const double UI_HANDLE_H    = UI_GRIP_H + 2.0 * UI_HANDLE_PAD + 2.0 * UI_HANDLE_MARG; // handle zone height
static const double UI_ICON_S   = 32.0;
static const double UI_GAP      = 4.0;
static const double UI_SV       = 90.0;   // color picker SV square
static const double UI_HUE_W    = 10.0;
static const double UI_SLIDER_W = 120.0;
static const double UI_SLIDER_H = 16.0;
static const double UI_CB       = 14.0;
static const double UI_STAMP_H  = 24.0;
static const double UI_SC       = 20.0;   // shortcut key box size

static bool pt_in_rect(ui_rect_t r, double x, double y) {
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void ui_compute_layout(struct escreen_state *state, bool vertical, ui_layout_t *L) {
	memset(L, 0, sizeof(*L));
	tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;
	bool color = tool->show_color;
	bool thick = tool->show_thickness;
	bool hard  = tool->show_hardness;
	bool fill  = tool->show_fill;
	bool stamp = tool->type == TOOL_STAMP;

	double x = UI_PAD, y = UI_HANDLE_H + UI_PAD;
	double max_x = UI_PAD, max_y = UI_HANDLE_H + UI_PAD;

	if (vertical) {
		// Icons in the middle, options to the RIGHT.
		double icon_x = UI_PAD;
		double ox = icon_x + UI_ICON_S + 8.0;

		y = UI_HANDLE_H + UI_PAD;
		for (int t = 0; t < TOOL_COUNT; t++) {
			L->icons[t] = (ui_rect_t){icon_x, y, UI_ICON_S, UI_ICON_S};
			y += UI_ICON_S + UI_GAP;
		}
		max_y = fmax(max_y, y - UI_GAP);
		max_x = fmax(max_x, ox);

		// Shortcuts to the LEFT of their icons, vertically aligned.
		for (int t = 0; t < TOOL_COUNT; t++) {
			if (!state->sketching.tools[t]->shortcut) continue;
			double sc_x = icon_x - UI_SC - 6.0;
			double sc_y = L->icons[t].y + (UI_ICON_S - UI_SC) / 2.0;
			L->shortcuts[t] = (ui_rect_t){sc_x, sc_y, UI_SC, UI_SC};
		}

		y = UI_HANDLE_H + UI_PAD;
		if (color) {
			L->wheel = (ui_rect_t){ox, y, UI_SV, UI_SV};
			L->hue   = (ui_rect_t){ox + UI_SV + 4.0, y, UI_HUE_W, UI_SV};
			y += UI_SV + 8.0;
			max_x = fmax(max_x, ox + UI_SV + 4.0 + UI_HUE_W);
		}
		if (thick) { L->sl_thick = (ui_rect_t){ox, y, UI_SLIDER_W, UI_SLIDER_H}; y += UI_SLIDER_H + 8.0; max_x = fmax(max_x, ox + UI_SLIDER_W); }
		if (hard)  { L->sl_hard  = (ui_rect_t){ox, y, UI_SLIDER_W, UI_SLIDER_H}; y += UI_SLIDER_H + 8.0; }
		if (fill)  { L->cb_fill  = (ui_rect_t){ox, y, UI_CB, UI_CB}; y += UI_CB + 8.0; }
		if (stamp) {
			L->stamp_minus = (ui_rect_t){ox, y, 22, UI_STAMP_H};
			L->stamp_num   = (ui_rect_t){ox + 26.0, y, 34, UI_STAMP_H};
			L->stamp_plus  = (ui_rect_t){ox + 64.0, y, 22, UI_STAMP_H};
			y += UI_STAMP_H + 8.0;
			max_x = fmax(max_x, ox + 86.0);
		}
		max_y = fmax(max_y, y - 8.0);
	} else {
		// Icons in the middle, options BELOW.
		double icon_y = UI_HANDLE_H + UI_PAD;

		x = UI_PAD;
		for (int t = 0; t < TOOL_COUNT; t++) {
			L->icons[t] = (ui_rect_t){x, icon_y, UI_ICON_S, UI_ICON_S};
			x += UI_ICON_S + UI_GAP;
		}
		max_x = fmax(max_x, x - UI_GAP);
		max_y = fmax(max_y, icon_y + UI_ICON_S);

		// Shortcuts ABOVE their icons, horizontally aligned.
		for (int t = 0; t < TOOL_COUNT; t++) {
			if (!state->sketching.tools[t]->shortcut) continue;
			double sc_x = L->icons[t].x + (UI_ICON_S - UI_SC) / 2.0;
			double sc_y = icon_y - UI_SC - 6.0;
			L->shortcuts[t] = (ui_rect_t){sc_x, sc_y, UI_SC, UI_SC};
		}

		double oy = icon_y + UI_ICON_S + 8.0;
		if (color) {
			L->wheel = (ui_rect_t){x, icon_y, UI_SV, UI_SV};
			L->hue   = (ui_rect_t){x + UI_SV + 4.0, icon_y, UI_HUE_W, UI_SV};
			x += UI_SV + 4.0 + UI_HUE_W + 8.0;
			max_x = fmax(max_x, x - 8.0);
			max_y = fmax(max_y, icon_y + UI_SV);
		}
		if (thick) { L->sl_thick = (ui_rect_t){x, oy, UI_SLIDER_W, UI_SLIDER_H}; oy += UI_SLIDER_H + 8.0; max_x = fmax(max_x, x + UI_SLIDER_W); }
		if (hard)  { L->sl_hard  = (ui_rect_t){x, oy, UI_SLIDER_W, UI_SLIDER_H}; oy += UI_SLIDER_H + 8.0; }
		if (fill)  { L->cb_fill  = (ui_rect_t){x, oy, UI_CB, UI_CB}; oy += UI_CB + 8.0; }
		if (stamp) {
			L->stamp_minus = (ui_rect_t){x, oy, 22, UI_STAMP_H};
			L->stamp_num   = (ui_rect_t){x + 26.0, oy, 34, UI_STAMP_H};
			L->stamp_plus  = (ui_rect_t){x + 64.0, oy, 22, UI_STAMP_H};
			oy += UI_STAMP_H + 8.0;
			max_x = fmax(max_x, x + 86.0);
		}
		max_y = fmax(max_y, oy - 8.0);
	}

	// Expand total size to include shortcut overhang.
	for (int t = 0; t < TOOL_COUNT; t++) {
		if (!state->sketching.tools[t]->shortcut) continue;
		double r = L->shortcuts[t].x + L->shortcuts[t].w;
		double b = L->shortcuts[t].y + L->shortcuts[t].h;
		if (r > max_x) max_x = r;
		if (b > max_y) max_y = b;
	}

	L->w = max_x + UI_PAD;
	L->h = max_y + UI_PAD;
	double hh = UI_HANDLE_H - UI_HANDLE_MARG * 2.0;
	L->handle = (ui_rect_t){ (L->w - UI_HANDLE_W) / 2.0, UI_HANDLE_MARG, UI_HANDLE_W, hh };
}

static void ui_layout_translate(ui_layout_t *L, double dx, double dy) {
	L->x = dx; L->y = dy;
	L->handle.x += dx; L->handle.y += dy;
	for (int t = 0; t < TOOL_COUNT; t++) { L->icons[t].x += dx; L->icons[t].y += dy; }
	for (int t = 0; t < TOOL_COUNT; t++) { L->shortcuts[t].x += dx; L->shortcuts[t].y += dy; }
	L->wheel.x += dx; L->wheel.y += dy;
	L->hue.x += dx; L->hue.y += dy;
	L->sl_thick.x += dx; L->sl_thick.y += dy;
	L->sl_hard.x += dx; L->sl_hard.y += dy;
	L->cb_fill.x += dx; L->cb_fill.y += dy;
	L->stamp_minus.x += dx; L->stamp_minus.y += dy;
	L->stamp_num.x += dx; L->stamp_num.y += dy;
	L->stamp_plus.x += dx; L->stamp_plus.y += dy;
}

static void get_toolbar_placement(struct escreen_state *state, double w_h, double h_h, double w_v, double h_v, double *out_x, double *out_y, bool *out_vertical) {
	// Find the monitor that contains the center of the selection
	struct escreen_output *o, *best = NULL;
	int cx = state->result.x + state->result.width / 2;
	int cy = state->result.y + state->result.height / 2;
	wl_list_for_each(o, &state->outputs, link) {
		if (cx >= o->logical_geometry.x && cx < o->logical_geometry.x + o->logical_geometry.width &&
			cy >= o->logical_geometry.y && cy < o->logical_geometry.y + o->logical_geometry.height) {
			best = o;
			break;
		}
	}

	double min_x = best ? best->logical_geometry.x : state->total_min_x;
	double max_x = best ? best->logical_geometry.x + best->logical_geometry.width : state->total_max_x;
	double min_y = best ? best->logical_geometry.y : state->total_min_y;
	double max_y = best ? best->logical_geometry.y + best->logical_geometry.height : state->total_max_y;

	double sx = (double)state->result.x;
	double sy = (double)state->result.y;
	double sw = (double)state->result.width;
	double sh = (double)state->result.height;

	auto check_placement = [&](double px, double py, double cw, double ch, double *res_x, double *res_y) -> bool {
		if (py < min_y + 4) py = min_y + 4;
		if (py + ch > max_y - 4) py = max_y - ch - 4;
		if (px < min_x + 4) px = min_x + 4;
		if (px + cw > max_x - 4) px = max_x - cw - 4;

		bool intersect_x = px < sx + sw && px + cw > sx;
		bool intersect_y = py < sy + sh && py + ch > sy;

		*res_x = px;
		*res_y = py;
		return !(intersect_x && intersect_y);
	};

	// Priority 1: Sides (Left/Right) - ALWAYS try Vertical first
	if (check_placement(sx - w_v - 12, sy + (sh - h_v) / 2.0, w_v, h_v, out_x, out_y)) { *out_vertical = true; return; }
	if (check_placement(sx + sw + 12, sy + (sh - h_v) / 2.0, w_v, h_v, out_x, out_y)) { *out_vertical = true; return; }

	// Priority 2: Bottom/Top - Try Horizontal first
	if (check_placement(sx + (sw - w_h) / 2.0, sy + sh + 12, w_h, h_h, out_x, out_y)) { *out_vertical = false; return; }
	if (check_placement(sx + (sw - w_h) / 2.0, sy - h_h - 12, w_h, h_h, out_x, out_y)) { *out_vertical = false; return; }

	// Priority 3: Bottom/Top - Fallback to Vertical if Horizontal didn't fit
	if (check_placement(sx + (sw - w_v) / 2.0, sy + sh + 12, w_v, h_v, out_x, out_y)) { *out_vertical = true; return; }
	if (check_placement(sx + (sw - w_v) / 2.0, sy - h_v - 12, w_v, h_v, out_x, out_y)) { *out_vertical = true; return; }

	// Final Fallback: forced vertical on side (might intersect)
	*out_vertical = true;
	*out_x = sx + sw + 12;
	*out_y = sy + 12;
	if (*out_x + w_v > max_x - 4) *out_x = sx - w_v - 12;
	if (*out_x < min_x + 4) *out_x = min_x + 4;
	if (*out_y + h_v > max_y - 4) *out_y = max_y - h_v - 4;
}

// Current toolbar layout at its on-screen position (auto-placed or pinned).
static void ui_get_current_layout(struct escreen_state *state, ui_layout_t *out) {
	ui_layout_t Lv, Lh;
	ui_compute_layout(state, true, &Lv);
	ui_compute_layout(state, false, &Lh);

	double x, y;
	bool vertical;
	get_toolbar_placement(state, Lh.w, Lh.h, Lv.w, Lv.h, &x, &y, &vertical);
	ui_layout_t *L = vertical ? &Lv : &Lh;

	if (state->sketching.toolbar_pinned) {
		x = state->sketching.toolbar_pinned_x;
		y = state->sketching.toolbar_pinned_y;
	}

	// Clamp inside the monitor containing the toolbar center.
	{
		double center_x = x + L->w * 0.5;
		double center_y = y + L->h * 0.5;
		struct escreen_output *o, *best = NULL;
		wl_list_for_each(o, &state->outputs, link) {
			if (center_x >= o->logical_geometry.x && center_x < o->logical_geometry.x + o->logical_geometry.width &&
			    center_y >= o->logical_geometry.y && center_y < o->logical_geometry.y + o->logical_geometry.height) {
				best = o;
				break;
			}
		}

		double mon_min_x = best ? best->logical_geometry.x : state->total_min_x;
		double mon_min_y = best ? best->logical_geometry.y : state->total_min_y;
		double mon_max_x = best ? best->logical_geometry.x + best->logical_geometry.width  : state->total_max_x;
		double mon_max_y = best ? best->logical_geometry.y + best->logical_geometry.height : state->total_max_y;

		double min_x = mon_min_x + 4.0;
		double max_x = mon_max_x - L->w - 4.0;
		double min_y = mon_min_y + 4.0;
		double max_y = mon_max_y - L->h - 4.0;

		if (max_x < min_x) max_x = min_x;
		if (max_y < min_y) max_y = min_y;

		if (x < min_x) x = min_x;
		if (x > max_x) x = max_x;
		if (y < min_y) y = min_y;
		if (y > max_y) y = max_y;

		if (state->sketching.toolbar_pinned) {
			state->sketching.toolbar_pinned_x = x;
			state->sketching.toolbar_pinned_y = y;
		}
	}

	ui_layout_translate(L, x, y);
	state->sketching.is_vertical = vertical;
	*out = *L;
}

// ---- drawing helpers --------------------------------------------------------

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > h / 2.0) r = h / 2.0;
	if (r > w / 2.0) r = w / 2.0;
	cairo_new_path(cr);
	cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_arc(cr, x + w - r, y + r, r, 1.5 * M_PI, 2 * M_PI);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * M_PI);
	cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
	cairo_close_path(cr);
}

static void fill_rounded(cairo_t *cr, double x, double y, double w, double h, double r,
		double R, double G, double B, double A) {
	rounded_rect_path(cr, x, y, w, h, r);
	cairo_set_source_rgba(cr, R, G, B, A);
	cairo_fill(cr);
}

static void stroke_rounded(cairo_t *cr, double x, double y, double w, double h, double r, double lw,
		double R, double G, double B, double A) {
	rounded_rect_path(cr, x, y, w, h, r);
	cairo_set_line_width(cr, lw);
	cairo_set_source_rgba(cr, R, G, B, A);
	cairo_stroke(cr);
}

static void draw_text(cairo_t *cr, double x, double y, const char *s, double size, double R, double G, double B, double A) {
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_set_source_rgba(cr, R, G, B, A);
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, s);
}

static void draw_centered_text(cairo_t *cr, double cx, double cy, const char *s, double size, double R, double G, double B, double A) {
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_text_extents_t te;
	cairo_text_extents(cr, s, &te);
	cairo_set_source_rgba(cr, R, G, B, A);
	cairo_move_to(cr, cx - te.width / 2.0 - te.x_bearing, cy - te.height / 2.0 - te.y_bearing);
	cairo_show_text(cr, s);
}

// 2xN grip-dot grid for the drag handle.
static void draw_grip_dots(cairo_t *cr, double x, double y, double w, double h, double R, double G, double B, double A) {
	const double cx = x + w * 0.5;
	const double cy = y + h * 0.5;
	cairo_set_source_rgba(cr, R, G, B, A);
	for (int col_i = -2; col_i <= 2; col_i++)
		for (int row_i = -1; row_i <= 1; row_i++) {
			cairo_arc(cr, cx + col_i * UI_GRIP_STEP, cy + row_i * UI_GRIP_STEP, UI_GRIP_R, 0, 2 * M_PI);
			cairo_fill(cr);
		}
}

// ---- tool icons --------------------------------------------------------------

static void draw_icon(cairo_t *cr, double cx, double cy, tool_type_t type, double R, double G, double B) {
	cairo_set_source_rgba(cr, R, G, B, 1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

	switch (type) {
	case TOOL_SELECT:
		cairo_rectangle(cr, cx - 8, cy - 8, 16, 16);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_rectangle(cr, cx - 10, cy - 10, 4, 4); cairo_fill(cr);
		cairo_rectangle(cr, cx + 6, cy - 10, 4, 4); cairo_fill(cr);
		cairo_rectangle(cr, cx - 10, cy + 6, 4, 4); cairo_fill(cr);
		cairo_rectangle(cr, cx + 6, cy + 6, 4, 4); cairo_fill(cr);
		break;
	case TOOL_BRUSH:
		cairo_move_to(cr, cx - 8, cy + 8);
		cairo_curve_to(cr, cx - 4, cy - 8, cx + 4, cy + 8, cx + 8, cy - 8);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		break;
	case TOOL_BLUR:
		cairo_move_to(cr, cx, cy - 8);
		cairo_line_to(cr, cx - 5, cy + 2);
		cairo_line_to(cr, cx + 5, cy + 2);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy + 3, 5, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case TOOL_LINE:
		cairo_move_to(cr, cx - 8, cy + 8);
		cairo_line_to(cr, cx + 8, cy - 8);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		break;
	case TOOL_RECTANGLE:
		cairo_rectangle(cr, cx - 10, cy - 6, 19, 11);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		break;
	case TOOL_ARROW:
		cairo_move_to(cr, cx - 8, cy + 8);
		cairo_line_to(cr, cx + 8, cy - 8);
		cairo_move_to(cr, cx + 8, cy - 8);
		cairo_line_to(cr, cx - 2, cy - 8);
		cairo_move_to(cr, cx + 8, cy - 8);
		cairo_line_to(cr, cx + 8, cy + 2);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		break;
	case TOOL_STAMP:
		cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
		cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		draw_centered_text(cr, cx + 0.5, cy + 1, "1", 12, 1, 1, 1, 1);
		break;
	case TOOL_TEXT:
		cairo_move_to(cr, cx - 7, cy - 7);
		cairo_line_to(cr, cx + 7, cy - 7);
		cairo_move_to(cr, cx, cy - 7);
		cairo_line_to(cr, cx, cy + 8);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		break;
	case TOOL_LASSO: {
		double r = 9.0;
		for (double a = -0.5; a < 5.0; a += 0.8) {
			cairo_arc(cr, cx + cos(a) * r, cy + sin(a) * (r * 0.7), 1.5, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		cairo_move_to(cr, cx + 8, cy);
		cairo_line_to(cr, cx + 12, cy + 4);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		break;
	}
	case TOOL_COLORPICKER:
		cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 3, cy);
		cairo_line_to(cr, cx + 4, cy);
		cairo_move_to(cr, cx, cy - 3);
		cairo_line_to(cr, cx, cy + 4);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_arc(cr, cx + 7, cy + 7, 3, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	default:
		break;
	}
}

static void draw_icon_button(struct escreen_state *state, cairo_t *cr, ui_rect_t r, tool_type_t type,
		bool is_active, bool hovered) {
	escreen_color_t accent = state->config.colors.accent;
	if (is_active) {
		fill_rounded(cr, r.x, r.y, r.w, r.h, 4, accent.r, accent.g, accent.b, accent.a);
	} else if (hovered) {
		escreen_color_t hv = state->config.colors.button_hover;
		fill_rounded(cr, r.x, r.y, r.w, r.h, 4, hv.r, hv.g, hv.b, hv.a);
	}
	draw_icon(cr, r.x + r.w / 2, r.y + r.h / 2, type, is_active ? 1.0 : 0.72, is_active ? 1.0 : 0.72, is_active ? 1.0 : 0.72);
}

// ---- option widgets -----------------------------------------------------------

static void draw_slider(struct escreen_state *state, cairo_t *cr, ui_rect_t r, double frac,
		const char *label, int widget, int hovered, int active) {
	escreen_color_t accent = state->config.colors.accent;
	bool hl = hovered == widget || active == widget;

	fill_rounded(cr, r.x, r.y, r.w, r.h, r.h / 2, 0.12, 0.12, 0.12, 1);
	if (frac > 0.0)
		fill_rounded(cr, r.x, r.y, r.w * frac, r.h, r.h / 2, accent.r, accent.g, accent.b, accent.a);
	if (hl)
		stroke_rounded(cr, r.x - 1.5, r.y - 1.5, r.w + 3, r.h + 3, r.h / 2 + 1.5, 1.5, accent.r, accent.g, accent.b, 0.9);

	double knob_x = r.x + frac * r.w;
	cairo_arc(cr, knob_x, r.y + r.h / 2, 6, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, 1.5);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	cairo_stroke(cr);

	draw_centered_text(cr, r.x + r.w / 2, r.y + r.h / 2, label, 11, 1, 1, 1, 0.95);
}

static void draw_checkbox(struct escreen_state *state, cairo_t *cr, ui_rect_t r, bool checked, bool hovered) {
	escreen_color_t accent = state->config.colors.accent;
	fill_rounded(cr, r.x, r.y, r.w, r.h, 3, 0.15, 0.15, 0.15, 1);
	if (checked) {
		fill_rounded(cr, r.x, r.y, r.w, r.h, 3, accent.r, accent.g, accent.b, 1);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_set_line_width(cr, 2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_move_to(cr, r.x + 3, r.y + r.h / 2);
		cairo_line_to(cr, r.x + r.w / 2, r.y + r.h - 3);
		cairo_line_to(cr, r.x + r.w - 2, r.y + 2);
		cairo_stroke(cr);
	} else {
		stroke_rounded(cr, r.x, r.y, r.w, r.h, 3, 1.5, accent.r, accent.g, accent.b, 0.7);
	}
	if (hovered)
		stroke_rounded(cr, r.x - 1.5, r.y - 1.5, r.w + 3, r.h + 3, 4, 1.5, accent.r, accent.g, accent.b, 0.9);
	draw_text(cr, r.x + r.w + 8, r.y + r.h - 3, "Fill", 12, 0.85, 0.85, 0.85, 1);
}

static void draw_mini_button(cairo_t *cr, ui_rect_t r, bool hovered, escreen_color_t accent, const char *label) {
	fill_rounded(cr, r.x, r.y, r.w, r.h, 4, 0.15, 0.15, 0.15, 1);
	if (hovered)
		fill_rounded(cr, r.x, r.y, r.w, r.h, 4, accent.r, accent.g, accent.b, 0.5);
	draw_centered_text(cr, r.x + r.w / 2, r.y + r.h / 2 + 0.5, label, 14, 1, 1, 1, 1);
}

static void draw_stamp_counter(struct escreen_state *state, cairo_t *cr, const ui_layout_t *L, int hovered) {
	int c = *tool_stamp_get_counter_ptr();
	escreen_color_t accent = state->config.colors.accent;

	draw_mini_button(cr, L->stamp_minus, hovered == UI_STAMP_MINUS, accent, "-");

	fill_rounded(cr, L->stamp_num.x, L->stamp_num.y, L->stamp_num.w, L->stamp_num.h, 4, 0.15, 0.15, 0.15, 1);
	stroke_rounded(cr, L->stamp_num.x, L->stamp_num.y, L->stamp_num.w, L->stamp_num.h, 4, 1.5, accent.r, accent.g, accent.b, 0.7);
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", c);
	draw_centered_text(cr, L->stamp_num.x + L->stamp_num.w / 2, L->stamp_num.y + L->stamp_num.h / 2, buf, 13, 1, 1, 1, 1);

	draw_mini_button(cr, L->stamp_plus, hovered == UI_STAMP_PLUS, accent, "+");
}

static void draw_wheel(struct escreen_state *state, cairo_t *cr, const ui_layout_t *L, int hovered, int active) {
	double h, s, v;
	rgb_to_hsv(state->sketching.r, state->sketching.g, state->sketching.b, &h, &s, &v);
	double hr, hg, hb;
	hsv_to_rgb(h, 1, 1, &hr, &hg, &hb);

	// SV square: white->pure hue horizontally, multiplied by black->white vertically.
	{
		double wx = L->wheel.x, wy = L->wheel.y, ww = L->wheel.w, wh = L->wheel.h;
		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_clip(cr);

		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_pattern_t *g1 = cairo_pattern_create_linear(wx, 0, wx + ww, 0);
		cairo_pattern_add_color_stop_rgba(g1, 0, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(g1, 1, hr, hg, hb, 1);
		cairo_set_source(cr, g1);
		cairo_fill(cr);
		cairo_pattern_destroy(g1);

		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_pattern_t *g2 = cairo_pattern_create_linear(0, wy, 0, wy + wh);
		cairo_pattern_add_color_stop_rgba(g2, 0, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(g2, 1, 0, 0, 0, 1);
		cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
		cairo_set_source(cr, g2);
		cairo_fill(cr);
		cairo_pattern_destroy(g2);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_reset_clip(cr);

		cairo_set_line_width(cr, 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_stroke(cr);

		// knob at current (s, v)
		double kx = wx + s * ww, ky = wy + (1.0 - v) * wh;
		cairo_arc(cr, kx, ky, 6, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 1.5);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
		cairo_stroke(cr);
	}

	// Hue strip (vertical, rainbow).
	{
		double hx = L->hue.x, hy = L->hue.y, hw = L->hue.w, hh = L->hue.h;
		cairo_rectangle(cr, hx, hy, hw, hh);
		cairo_pattern_t *gp = cairo_pattern_create_linear(0, hy, 0, hy + hh);
		for (int i = 0; i <= 6; i++) {
			double R, G, B;
			hsv_to_rgb(i / 6.0, 1, 1, &R, &G, &B);
			cairo_pattern_add_color_stop_rgba(gp, i / 6.0, R, G, B, 1);
		}
		cairo_set_source(cr, gp);
		cairo_fill(cr);
		cairo_pattern_destroy(gp);

		cairo_set_line_width(cr, 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
		cairo_rectangle(cr, hx, hy, hw, hh);
		cairo_stroke(cr);

		double iy = hy + h * hh;
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_rectangle(cr, hx, iy - 1.5, hw, 3);
		cairo_fill(cr);
	}

	escreen_color_t accent = state->config.colors.accent;
	if (hovered == UI_WHEEL || active == UI_WHEEL)
		stroke_rounded(cr, L->wheel.x - 2, L->wheel.y - 2, L->wheel.w + 4, L->wheel.h + 4, 4, 1.5, accent.r, accent.g, accent.b, 0.9);
	if (hovered == UI_HUE || active == UI_HUE)
		stroke_rounded(cr, L->hue.x - 2, L->hue.y - 2, L->hue.w + 4, L->hue.h + 4, 2, 1.5, accent.r, accent.g, accent.b, 0.9);
}

// ---- tooltip -----------------------------------------------------------------

static const char *ui_tooltip_for(struct escreen_state *state, int widget) {
	switch (widget) {
	case UI_HANDLE:         return "Drag to pin\nDouble-click to auto-place";
	case UI_WHEEL:          return "Color";
	case UI_HUE:            return "Hue";
	case UI_SLIDER_THICK:   return "Size";
	case UI_SLIDER_HARD:    return "Hardness";
	case UI_CHECKBOX_FILL:  return "Fill";
	case UI_STAMP_MINUS:    return "Previous number";
	case UI_STAMP_PLUS:     return "Next number";
	default:
		if (widget >= UI_ICON_BASE && widget < UI_ICON_BASE + (int)TOOL_COUNT)
			return state->sketching.tools[widget - UI_ICON_BASE]->name;
		if (widget >= UI_SHORTCUT_BASE && widget < UI_SHORTCUT_BASE + (int)TOOL_COUNT) {
			int t = widget - UI_SHORTCUT_BASE;
			tool_interface_t *tool = state->sketching.tools[t];
			if (tool && tool->shortcut) {
				static char tip[32];
				snprintf(tip, sizeof(tip), "%c - %s", tool->shortcut, tool->name);
				return tip;
			}
		}
		return NULL;
	}
}

static ui_rect_t ui_rect_of(const ui_layout_t *L, int widget) {
	if (widget == UI_HANDLE) return L->handle;
	if (widget >= UI_ICON_BASE && widget < UI_ICON_BASE + (int)TOOL_COUNT) return L->icons[widget - UI_ICON_BASE];
	if (widget >= UI_SHORTCUT_BASE && widget < UI_SHORTCUT_BASE + (int)TOOL_COUNT) return L->shortcuts[widget - UI_SHORTCUT_BASE];
	if (widget == UI_WHEEL) return L->wheel;
	if (widget == UI_HUE) return L->hue;
	if (widget == UI_SLIDER_THICK) return L->sl_thick;
	if (widget == UI_SLIDER_HARD) return L->sl_hard;
	if (widget == UI_CHECKBOX_FILL) return L->cb_fill;
	if (widget == UI_STAMP_MINUS) return L->stamp_minus;
	if (widget == UI_STAMP_PLUS) return L->stamp_plus;
	ui_rect_t z = {0, 0, 0, 0};
	return z;
}

static void draw_tooltip(struct escreen_state *state, cairo_t *cr, const ui_layout_t *L, int widget) {
	const char *text = ui_tooltip_for(state, widget);
	if (!text) return;

	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12);

	// Split into lines and find the widest.
	const double lh = 16.0;
	double tw = 0.0;
	int lines = 0;
	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
		cairo_text_extents_t te;
		cairo_text_extents(cr, line, &te);
		if (te.width > tw) tw = te.width;
		lines++;
	}
	double pad = 5.0;
	double tip_w = tw + pad * 2;
	double tip_h = lines * lh + pad * 2;

	ui_rect_t r = ui_rect_of(L, widget);
	double tx = r.x + r.w + 8.0;
	double ty = r.y;
	double min_x = state->total_min_x + 4, max_x = state->total_max_x - tip_w - 4;
	double min_y = state->total_min_y + 4, max_y = state->total_max_y - tip_h - 4;
	if (max_x < min_x) max_x = min_x;
	if (max_y < min_y) max_y = min_y;
	if (tx < min_x) tx = min_x;
	if (tx > max_x) tx = max_x - r.w - 8.0;
	if (tx < min_x) tx = min_x;
	if (ty < min_y) ty = min_y;
	if (ty > max_y) ty = max_y;

	fill_rounded(cr, tx, ty, tip_w, tip_h, 4, 0.05, 0.05, 0.05, 0.9);
	stroke_rounded(cr, tx, ty, tip_w, tip_h, 4, 1, 1, 1, 1, 0.35);

	double ly = ty + pad + lh - 4.0;
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
		draw_text(cr, tx + pad, ly, line, 12, 1, 1, 1, 1);
		ly += lh;
	}
}

// ---- hit-testing & interaction -------------------------------------------------

static int ui_widget_at(struct escreen_state *state, const ui_layout_t *L, double mx, double my) {
	tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;
	if (pt_in_rect(L->handle, mx, my)) return UI_HANDLE;
	for (int t = 0; t < TOOL_COUNT; t++)
		if (pt_in_rect(L->icons[t], mx, my)) return UI_ICON(t);
	for (int t = 0; t < TOOL_COUNT; t++)
		if (state->sketching.tools[t]->shortcut && pt_in_rect(L->shortcuts[t], mx, my)) return UI_SHORTCUT_BASE + t;
	if (tool->show_color) {
		if (pt_in_rect(L->hue, mx, my)) return UI_HUE;
		if (pt_in_rect(L->wheel, mx, my)) return UI_WHEEL;
	}
	if (tool->show_thickness && pt_in_rect(L->sl_thick, mx, my)) return UI_SLIDER_THICK;
	if (tool->show_hardness && pt_in_rect(L->sl_hard, mx, my)) return UI_SLIDER_HARD;
	if (tool->show_fill && pt_in_rect(L->cb_fill, mx, my)) return UI_CHECKBOX_FILL;
	if (tool->type == TOOL_STAMP) {
		if (pt_in_rect(L->stamp_minus, mx, my)) return UI_STAMP_MINUS;
		if (pt_in_rect(L->stamp_plus, mx, my)) return UI_STAMP_PLUS;
	}
	return UI_NONE;
}

static void ui_slider_set(struct escreen_state *state, const ui_layout_t *L, int widget, double mx) {
	double frac = clamp01((mx - (widget == UI_SLIDER_THICK ? L->sl_thick.x : L->sl_hard.x)) /
	                      (widget == UI_SLIDER_THICK ? L->sl_thick.w : L->sl_hard.w));
	if (widget == UI_SLIDER_THICK) state->sketching.thickness = 1.0 + frac * 99.0;
	else state->sketching.hardness = frac;
}

static void ui_wheel_pick(struct escreen_state *state, const ui_layout_t *L, double mx, double my, int widget) {
	double h, s, v;
	rgb_to_hsv(state->sketching.r, state->sketching.g, state->sketching.b, &h, &s, &v);
	if (widget == UI_WHEEL) {
		s = clamp01((mx - L->wheel.x) / L->wheel.w);
		v = 1.0 - clamp01((my - L->wheel.y) / L->wheel.h);
	} else {
		h = clamp01((my - L->hue.y) / L->hue.h);
	}
	hsv_to_rgb(h, s, v, &state->sketching.r, &state->sketching.g, &state->sketching.b);
}

static void ui_get_mouse(struct escreen_state *state, double *x, double *y) {
	// Only the first seat drives the toolbar (single-pointer assumption).
	struct escreen_seat *seat = wl_container_of(state->seats.next, seat, link);
	*x = seat->x;
	*y = seat->y;
}

// ---- main UI entry point -------------------------------------------------------

void tools_draw_ui(struct escreen_state *state, cairo_t *cr) {
	if (!state->sketching.active_tool) return;
	tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;

	ui_layout_t L;
	ui_get_current_layout(state, &L);

	double mx, my;
	ui_get_mouse(state, &mx, &my);
	int hovered = state->sketching.ui_active ? UI_NONE : ui_widget_at(state, &L, mx, my);
	int active = state->sketching.ui_active;

	// Background
	fill_rounded(cr, L.x, L.y, L.w, L.h, 6, state->config.colors.toolbar_bg.r, state->config.colors.toolbar_bg.g, state->config.colors.toolbar_bg.b, state->config.colors.toolbar_bg.a);

	// Drag handle
	{
		bool held = state->sketching.toolbar_dragging;
		bool hl = held || hovered == UI_HANDLE;
		if (hl)
			fill_rounded(cr, L.handle.x, L.handle.y, L.handle.w, L.handle.h, 6, 1, 1, 1, held ? 0.16 : 0.08);
		double a = held ? 0.82 : (hl ? 0.55 : 0.31);
		draw_grip_dots(cr, L.handle.x, L.handle.y, L.handle.w, L.handle.h, 0.78, 0.78, 0.78, a);
	}

	// Tool icons
	for (int t = 0; t < TOOL_COUNT; t++) {
		bool is_active = (tool == state->sketching.tools[t]);
		bool hv = hovered == UI_ICON(t);
		draw_icon_button(state, cr, L.icons[t], (tool_type_t)t, is_active, hv);
	}

	// Shortcut keys
	for (int t = 0; t < TOOL_COUNT; t++) {
		if (!state->sketching.tools[t]->shortcut) continue;
		bool is_active = (tool->type == (tool_type_t)t);
		bool hv = hovered == UI_SHORTCUT_BASE + t;
		ui_rect_t r = L.shortcuts[t];

		if (is_active) {
			fill_rounded(cr, r.x, r.y, r.w, r.h, 3, state->config.colors.accent.r, state->config.colors.accent.g, state->config.colors.accent.b, state->config.colors.accent.a);
		} else if (hv) {
			fill_rounded(cr, r.x, r.y, r.w, r.h, 3, state->config.colors.button_hover.r, state->config.colors.button_hover.g, state->config.colors.button_hover.b, state->config.colors.button_hover.a);
		} else {
			fill_rounded(cr, r.x, r.y, r.w, r.h, 3, 0.15, 0.15, 0.15, 1);
		}

		char key_str[2] = {state->sketching.tools[t]->shortcut, '\0'};
		draw_centered_text(cr, r.x + r.w / 2, r.y + r.h / 2, key_str, 11,
			is_active ? 1.0 : 0.72, is_active ? 1.0 : 0.72, is_active ? 1.0 : 0.72, 1.0);
	}

	// Options
	if (tool->show_color)
		draw_wheel(state, cr, &L, hovered, active);
	if (tool->show_thickness) {
		char label[32];
		snprintf(label, sizeof(label), "Size: %.0f", state->sketching.thickness);
		draw_slider(state, cr, L.sl_thick, (state->sketching.thickness - 1.0) / 99.0, label, UI_SLIDER_THICK, hovered, active);
	}
	if (tool->show_hardness) {
		char label[32];
		snprintf(label, sizeof(label), "Hard: %.2f", state->sketching.hardness);
		draw_slider(state, cr, L.sl_hard, state->sketching.hardness, label, UI_SLIDER_HARD, hovered, active);
	}
	if (tool->show_fill)
		draw_checkbox(state, cr, L.cb_fill, state->sketching.filled, hovered == UI_CHECKBOX_FILL);
	if (tool->type == TOOL_STAMP)
		draw_stamp_counter(state, cr, &L, hovered);

	// Tooltip
	if (hovered != UI_NONE && active == UI_NONE)
		draw_tooltip(state, cr, &L, hovered);
}

bool tools_is_on_toolbar(struct escreen_state *state, double x, double y) {
	ui_layout_t L;
	ui_get_current_layout(state, &L);
	return x >= L.x && x < L.x + L.w && y >= L.y && y < L.y + L.h;
}

void tools_handle_button(struct escreen_state *state, double x, double y, bool pressed) {
	tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;

	if (pressed) {
		if (tools_is_on_toolbar(state, x, y)) {
			ui_layout_t L;
			ui_get_current_layout(state, &L);
			int w = ui_widget_at(state, &L, x, y);

			if (w == UI_HANDLE) {
				uint64_t now = ui_get_ms();
				bool dbl = now - state->sketching.toolbar_last_click_ms < 400 &&
				           fabs(x - state->sketching.toolbar_last_click_x) < 4 &&
				           fabs(y - state->sketching.toolbar_last_click_y) < 4;
				state->sketching.toolbar_last_click_ms = now;
				state->sketching.toolbar_last_click_x = x;
				state->sketching.toolbar_last_click_y = y;
				if (dbl) {
					state->sketching.toolbar_pinned = false;
					state->sketching.toolbar_dragging = false;
					state->sketching.ui_active = UI_NONE;
				} else {
					state->sketching.toolbar_dragging = true;
					state->sketching.toolbar_pinned = true;
					state->sketching.toolbar_pinned_x = L.x;
					state->sketching.toolbar_pinned_y = L.y;
					state->sketching.toolbar_drag_ox = x - L.x;
					state->sketching.toolbar_drag_oy = y - L.y;
				}
			} else if (w >= UI_ICON_BASE && w < UI_ICON_BASE + (int)TOOL_COUNT) {
				tools_set_active(state, (tool_type_t)(w - UI_ICON_BASE));
			} else if (w >= UI_SHORTCUT_BASE && w < UI_SHORTCUT_BASE + (int)TOOL_COUNT) {
				int t = w - UI_SHORTCUT_BASE;
				tools_set_active(state, (tool_type_t)t);
			} else if (w == UI_WHEEL || w == UI_HUE) {
				state->sketching.ui_active = w;
				ui_wheel_pick(state, &L, x, y, w);
			} else if (w == UI_SLIDER_THICK || w == UI_SLIDER_HARD) {
				state->sketching.ui_active = w;
				ui_slider_set(state, &L, w, x);
			} else if (w == UI_CHECKBOX_FILL) {
				state->sketching.filled = !state->sketching.filled;
			} else if (w == UI_STAMP_MINUS) {
				int *c = tool_stamp_get_counter_ptr();
				if (*c > 1) (*c)--;
			} else if (w == UI_STAMP_PLUS) {
				(*tool_stamp_get_counter_ptr())++;
			}
			return;
		}

		// Not on the toolbar: let the active tool have the click.
		state->sketching.drawing = true;
		if (tool && tool->on_mousedown) tool->on_mousedown(state, x, y);
	} else {
		// Release: end any toolbar interaction first.
		if (state->sketching.ui_active != UI_NONE || state->sketching.toolbar_dragging) {
			state->sketching.ui_active = UI_NONE;
			state->sketching.toolbar_dragging = false;
			return;
		}
		if (state->sketching.drawing && tool && tool->on_mouseup) tool->on_mouseup(state, x, y);
		state->sketching.drawing = false;
	}
}

void tools_handle_motion(struct escreen_state *state, double x, double y) {
	// Toolbar drag (pinning) and widget drags take priority over the tool.
	if (state->sketching.toolbar_dragging) {
		state->sketching.toolbar_pinned_x = x - state->sketching.toolbar_drag_ox;
		state->sketching.toolbar_pinned_y = y - state->sketching.toolbar_drag_oy;
		return;
	}

	int w = state->sketching.ui_active;
	if (w == UI_WHEEL || w == UI_HUE) {
		ui_layout_t L;
		ui_get_current_layout(state, &L);
		ui_wheel_pick(state, &L, x, y, w);
		return;
	}
	if (w == UI_SLIDER_THICK || w == UI_SLIDER_HARD) {
		ui_layout_t L;
		ui_get_current_layout(state, &L);
		ui_slider_set(state, &L, w, x);
		return;
	}

	if (state->sketching.drawing && state->sketching.active_tool && state->sketching.active_tool->on_mousemove) {
		state->sketching.active_tool->on_mousemove(state, x, y);
	}
}

// ---- tool manager ---------------------------------------------------------------

void tools_init(struct escreen_state *state) {
	state->sketching.tools[TOOL_SELECT]      = &tool_select;
	state->sketching.tools[TOOL_BRUSH]       = &tool_brush;
	state->sketching.tools[TOOL_BLUR]        = &tool_blur;
	state->sketching.tools[TOOL_LINE]        = &tool_line;
	state->sketching.tools[TOOL_RECTANGLE]   = &tool_rectangle;
	state->sketching.tools[TOOL_ARROW]       = &tool_arrow;
	state->sketching.tools[TOOL_STAMP]       = &tool_stamp;
	state->sketching.tools[TOOL_TEXT]        = &tool_text;
	state->sketching.tools[TOOL_LASSO]       = &tool_lasso;
	state->sketching.tools[TOOL_COLORPICKER] = &tool_colorpicker;

	state->sketching.active_tool = state->sketching.tools[TOOL_SELECT];
	state->sketching.text_buffer[0] = '\0';

	state->sketching.r = 1.0f; state->sketching.g = 0.0f; state->sketching.b = 0.0f; state->sketching.a = 1.0f;
	state->sketching.thickness = 5.0f;
	state->sketching.hardness = 0.5f;
	state->sketching.filled = false;
	state->sketching.is_vertical = false;

	state->sketching.history_count = 0;
	state->sketching.history_capacity = 16;
	state->sketching.history = (action_t*)calloc(state->sketching.history_capacity, sizeof(action_t));

	state->sketching.drawing = false;
}

void tools_cleanup(struct escreen_state *state) {
	for (size_t i = 0; i < state->sketching.history_count; i++) {
		if (state->sketching.history[i].points) free(state->sketching.history[i].points);
		if (state->sketching.history[i].text) free(state->sketching.history[i].text);
	}
	free(state->sketching.history);
	if (state->sketching.history_layer) {
		cairo_surface_destroy(state->sketching.history_layer);
		state->sketching.history_layer = NULL;
	}
	if (state->sketching.lasso_points) free(state->sketching.lasso_points);
}

extern "C" {

void tools_update_history(struct escreen_state *state) {
	if (!state->sketching.history_layer && state->global_capture) {
		int w = cairo_image_surface_get_width(state->global_capture);
		int h = cairo_image_surface_get_height(state->global_capture);
		state->sketching.history_layer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	}

	if (state->sketching.history_layer) {
		if (state->sketching.history_rendered_count != state->sketching.history_undo_pos) {
			cairo_t *lcr = cairo_create(state->sketching.history_layer);

			if (state->sketching.history_undo_pos < state->sketching.history_rendered_count) {
				cairo_set_operator(lcr, CAIRO_OPERATOR_CLEAR);
				cairo_paint(lcr);
				cairo_set_operator(lcr, CAIRO_OPERATOR_OVER);
				state->sketching.history_rendered_count = 0;
			}

			// Use max_scale_factor (true physical/logical ratio) — not o->scale which is
			// always 1 for fractional scaling and would place strokes in the wrong position.
			cairo_scale(lcr, state->max_scale_factor, state->max_scale_factor);
			cairo_translate(lcr, -state->total_min_x, -state->total_min_y);

			for (size_t i = state->sketching.history_rendered_count; i < state->sketching.history_undo_pos; i++) {
				action_t *action = &state->sketching.history[i];
				state->sketching.tools[action->type]->render_action(state, lcr, action);
			}
			cairo_destroy(lcr);
			state->sketching.history_rendered_count = state->sketching.history_undo_pos;
		}
	}
}

void tools_draw(struct escreen_state *state, cairo_t *cr) {
	if ((state->sketching.drawing || state->sketching.is_text_editing) &&
	    state->sketching.active_tool && state->sketching.active_tool->draw_preview) {
		state->sketching.active_tool->draw_preview(state, cr);
	}
}

void tools_set_active(struct escreen_state *state, tool_type_t type) {
	if (type < TOOL_COUNT && state->sketching.active_tool != state->sketching.tools[type]) {
		state->sketching.active_tool = state->sketching.tools[type];
		state->sketching.ui_layout_frames = 12; // Force 12 frames to settle layout across all outputs
	}
}

void tools_add_action(struct escreen_state *state, action_t action) {
	for (size_t i = state->sketching.history_undo_pos; i < state->sketching.history_count; i++) {
		if (state->sketching.history[i].points) free(state->sketching.history[i].points);
		if (state->sketching.history[i].text) free(state->sketching.history[i].text);
	}
	state->sketching.history_count = state->sketching.history_undo_pos;

	if (state->sketching.history_count >= state->sketching.history_capacity) {
		state->sketching.history_capacity *= 2;
		state->sketching.history = (action_t*)realloc(state->sketching.history, state->sketching.history_capacity * sizeof(action_t));
	}
	state->sketching.history[state->sketching.history_count++] = action;
	state->sketching.history_undo_pos = state->sketching.history_count;
}

static bool is_word_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

void tools_handle_key(struct escreen_state *state, uint32_t sym, const char *utf8, bool shift_down, bool ctrl_down) {
	if (!state->sketching.is_text_editing) return;

	char *buf = state->sketching.text_buffer;
	int *pos = &state->sketching.text_cursor_pos;
	size_t len = strlen(buf);

	if (sym == XKB_KEY_Return) {
		if (shift_down) {
			// Insert newline at cursor
			if (len + 1 < sizeof(state->sketching.text_buffer)) {
				memmove(buf + *pos + 1, buf + *pos, len - *pos + 1);
				buf[*pos] = '\n';
				(*pos)++;
			}
			return;
		}
		// Commit
		if (buf[0] != '\0') {
			action_t action = {};
			action.type = TOOL_TEXT;
			action.r = state->sketching.r;
			action.g = state->sketching.g;
			action.b = state->sketching.b;
			action.a = state->sketching.a;
			action.thickness = state->sketching.thickness;
			action.x1 = state->sketching.text_x;
			action.y1 = state->sketching.text_y;
			action.text = strdup(buf);
			tools_add_action(state, action);
		}
		state->sketching.is_text_editing = false;
	} else if (sym == XKB_KEY_Escape) {
		state->sketching.is_text_editing = false;
		buf[0] = '\0';
		*pos = 0;
	} else if (sym == XKB_KEY_BackSpace) {
		if (*pos > 0) {
			int to_delete = 1;
			if (ctrl_down) {
				int i = *pos - 1;
				while (i > 0 && !is_word_char(buf[i])) i--;
				while (i > 0 && is_word_char(buf[i])) i--;
				if (i > 0) i++; // stay after the non-word char
				to_delete = *pos - i;
			} else {
				// UTF-8 aware single backspace
				int i = *pos - 1;
				while (i > 0 && (buf[i] & 0xC0) == 0x80) i--;
				to_delete = *pos - i;
			}
			memmove(buf + *pos - to_delete, buf + *pos, len - *pos + 1);
			*pos -= to_delete;
		}
	} else if (sym == XKB_KEY_Delete) {
		if ((size_t)*pos < len) {
			int to_delete = 1;
			if (ctrl_down) {
				int i = *pos;
				while ((size_t)i < len && !is_word_char(buf[i])) i++;
				while ((size_t)i < len && is_word_char(buf[i])) i++;
				to_delete = i - *pos;
			} else {
				// UTF-8 aware single delete
				int i = *pos + 1;
				while ((size_t)i < len && (buf[i] & 0xC0) == 0x80) i++;
				to_delete = i - *pos;
			}
			memmove(buf + *pos, buf + *pos + to_delete, len - (*pos + to_delete) + 1);
		}
	} else if (sym == XKB_KEY_Left) {
		if (*pos > 0) {
			if (ctrl_down) {
				int i = *pos - 1;
				while (i > 0 && !is_word_char(buf[i])) i--;
				while (i > 0 && is_word_char(buf[i])) i--;
				if (i > 0) i++;
				*pos = i;
			} else {
				int i = *pos - 1;
				while (i > 0 && (buf[i] & 0xC0) == 0x80) i--;
				*pos = i;
			}
		}
	} else if (sym == XKB_KEY_Right) {
		if ((size_t)*pos < len) {
			if (ctrl_down) {
				int i = *pos;
				while ((size_t)i < len && !is_word_char(buf[i])) i++;
				while ((size_t)i < len && is_word_char(buf[i])) i++;
				*pos = i;
			} else {
				int i = *pos + 1;
				while ((size_t)i < len && (buf[i] & 0xC0) == 0x80) i++;
				*pos = i;
			}
		}
	} else if (utf8 && utf8[0] != '\0') {
		size_t utf8_len = strlen(utf8);
		if (len + utf8_len < sizeof(state->sketching.text_buffer)) {
			memmove(buf + *pos + utf8_len, buf + *pos, len - *pos + 1);
			memcpy(buf + *pos, utf8, utf8_len);
			*pos += utf8_len;
		}
	}
}

bool tools_handle_shortcut_key(struct escreen_state *state, uint32_t sym) {
	char key = '\0';
	if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
		key = sym - XKB_KEY_a + 'A';
	} else if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
		key = sym - XKB_KEY_A + 'A';
	}

	if (key == '\0') return false;

	if (key == 'F') {
		tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;
		if (tool && tool->show_fill) {
			state->sketching.filled = !state->sketching.filled;
			return true;
		}
		return false;
	}

	for (int t = 0; t < TOOL_COUNT; t++) {
		if (state->sketching.tools[t]->shortcut == key) {
			tools_set_active(state, (tool_type_t)t);
			return true;
		}
	}

	return false;
}

} // extern "C"
