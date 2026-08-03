#include "imgui.h"
#include "imgui_internal.h"
#include "escreen.h"
#include "tools.h"
#include <vector>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>

extern void ImGui_ImplCairo_RenderDrawData(cairo_t* cr, ImDrawData* draw_data);
extern void ImGui_ImplCairo_CreateFontsTexture();
extern void ImGui_ImplCairo_DestroyFontsTexture();

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
	false, false, false, false, // UI flags: color, thickness, hardness, fill
	NULL, NULL, NULL, NULL, NULL, NULL // Callbacks including on_draw_preview
};

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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	
	ImGui_ImplCairo_CreateFontsTexture();
}


void tools_cleanup(struct escreen_state *state) {
	ImGui_ImplCairo_DestroyFontsTexture();
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
	ImGui::DestroyContext();
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

// Actual toolbar sizes measured from the last drawn frame (from the real
// widget layout). Placement uses these instead of guessed constants so the
// panel hugs the selection for the active tool's true width, which changes
// with the tool's options.
static double g_toolbar_v_w = 0.0, g_toolbar_v_h = 0.0; // vertical panel size
static double g_toolbar_h_w = 0.0, g_toolbar_h_h = 0.0; // horizontal panel size

static void get_toolbar_rect(struct escreen_state *state, double *x, double *y, double *w, double *h) {
	double w_v = g_toolbar_v_w, h_v = g_toolbar_v_h;
	double w_h = g_toolbar_h_w, h_h = g_toolbar_h_h;

	// Nothing has been drawn yet (very first frame): fall back to a panel
	// sized for an icon column next to the colour wheel. The measured size
	// replaces this as soon as the toolbar renders once.
	if (w_v <= 0.0) { w_v = 216.0; h_v = 392.0; }
	if (w_h <= 0.0) { w_h = 380.0; h_h = 60.0; }

	get_toolbar_placement(state, w_h, h_h, w_v, h_v, x, y, &state->sketching.is_vertical);
	
	if (state->sketching.is_vertical) {
		*w = w_v; *h = h_v;
	} else {
		*w = w_h; *h = h_h;
	}
}

bool tools_is_on_toolbar(struct escreen_state *state, double x, double y) {
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
		return true;
	}
	double tx, ty, tw, th;
	get_toolbar_rect(state, &tx, &ty, &tw, &th);
	return (x >= tx && x < tx + tw && y >= ty && y < ty + th);
}

static bool IconButton(struct escreen_state *state, const char* tooltip, tool_type_t type, bool is_active) {
    ImVec2 size(32, 32);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(tooltip, size);
    bool hovered = ImGui::IsItemHovered();
    
    if (hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImColor bg_col = is_active ? ImColor((float)state->config.colors.accent.r, (float)state->config.colors.accent.g, (float)state->config.colors.accent.b, (float)state->config.colors.accent.a) : ImColor(0, 0, 0, 0);
	ImColor fg_col = is_active ? ImColor(1.0f, 1.0f, 1.0f, 1.0f) : ImColor(0.7f, 0.7f, 0.7f, 1.0f);
	
	if (hovered && !is_active) bg_col = ImColor((float)state->config.colors.button_hover.r, (float)state->config.colors.button_hover.g, (float)state->config.colors.button_hover.b, (float)state->config.colors.button_hover.a);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(pos, ImVec2(pos.x + 32, pos.y + 32), bg_col, 4.0f);
    
    ImVec2 c = ImVec2(pos.x + size.x/2, pos.y + size.y/2);
    if (type == TOOL_SELECT) {
        draw->AddRect(ImVec2(c.x - 8, c.y - 8), ImVec2(c.x + 8, c.y + 8), fg_col, 0, 0, 1.5f);
        draw->AddRectFilled(ImVec2(c.x - 10, c.y - 10), ImVec2(c.x - 6, c.y - 6), fg_col);
        draw->AddRectFilled(ImVec2(c.x + 6, c.y + 6), ImVec2(c.x + 10, c.y + 10), fg_col);
        draw->AddRectFilled(ImVec2(c.x - 10, c.y + 6), ImVec2(c.x - 6, c.y + 10), fg_col);
        draw->AddRectFilled(ImVec2(c.x + 6, c.y - 10), ImVec2(c.x + 10, c.y - 6), fg_col);
    } else if (type == TOOL_BRUSH) {
        draw->AddBezierCubic(ImVec2(c.x - 8, c.y + 8), ImVec2(c.x - 4, c.y - 8), 
                             ImVec2(c.x + 4, c.y + 8), ImVec2(c.x + 8, c.y - 8), fg_col, 2.0f);
    } else if (type == TOOL_BLUR) {
        draw->AddTriangleFilled(ImVec2(c.x, c.y - 8), ImVec2(c.x - 5, c.y + 2), ImVec2(c.x + 5, c.y + 2), fg_col);
        draw->AddCircleFilled(ImVec2(c.x, c.y + 3), 5.0f, fg_col);
    } else if (type == TOOL_LINE) {
        draw->AddLine(ImVec2(c.x - 8, c.y + 8), ImVec2(c.x + 8, c.y - 8), fg_col, 2.0f);
    } else if (type == TOOL_RECTANGLE) {
        draw->AddRect(ImVec2(c.x - 10, c.y - 6), ImVec2(c.x + 9, c.y + 5), fg_col, 0, 0, 2.0f);
    } else if (type == TOOL_ARROW) {
        draw->AddLine(ImVec2(c.x - 8, c.y + 8), ImVec2(c.x + 8, c.y - 8), fg_col, 2.0f);
        draw->AddLine(ImVec2(c.x + 8, c.y - 8), ImVec2(c.x - 2, c.y - 8), fg_col, 2.0f);
        draw->AddLine(ImVec2(c.x + 8, c.y - 8), ImVec2(c.x + 8, c.y + 2), fg_col, 2.0f);
    } else if (type == TOOL_STAMP) {
        draw->AddCircleFilled(ImVec2(c.x, c.y), 8.0f, fg_col);
        draw->AddText(ImVec2(c.x - 3, c.y - 7), IM_COL32(255, 255, 255, 255), "1");
    } else if (type == TOOL_TEXT) {
        // Clean 'T' icon: top bar and stem only
        draw->AddLine(ImVec2(c.x - 7, c.y - 7), ImVec2(c.x + 7, c.y - 7), fg_col, 2.0f); // Top bar
        draw->AddLine(ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 8), fg_col, 2.0f);         // Stem
    } else if (type == TOOL_LASSO) {
        // Lasso icon: a dotted partial loop
        float r = 9.0f;
        for (float a = -0.5f; a < 5.0f; a += 0.8f) { // partial ellipse loop
            draw->AddCircleFilled(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * (r * 0.7f)), 1.5f, fg_col);
        }
        // Small "rope end" detail
        draw->AddLine(ImVec2(c.x + 8, c.y), ImVec2(c.x + 12, c.y + 4), fg_col, 1.5f);
    } else if (type == TOOL_COLORPICKER) {
        // Colorpicker icon: a scope ring with a central cross and a solid droplet 
        draw->AddCircle(c, 7.0f, fg_col, 0, 1.5f);
        draw->AddLine(ImVec2(c.x - 3, c.y), ImVec2(c.x + 4, c.y), fg_col, 1.5f);
        draw->AddLine(ImVec2(c.x, c.y - 3), ImVec2(c.x, c.y + 4), fg_col, 1.5f);
        draw->AddCircleFilled(ImVec2(c.x + 7, c.y + 7), 3.0f, fg_col);
    }
    
    return clicked;
}

// ---- Drag handle helpers -------------------------------------------------

// Draw a 2×N or N×2 dot-grid grip in the centre of [rect_min, rect_max].
static void draw_grip_dots(ImDrawList *draw,
                           ImVec2 rect_min, ImVec2 rect_max,
                           bool horizontal, ImU32 col)
{
	const float cx = (rect_min.x + rect_max.x) * 0.5f;
	const float cy = (rect_min.y + rect_max.y) * 0.5f;
	const float STEP = 4.0f;
	const float DOT_R = 1.4f;

	if (horizontal) {
		// Two rows, five columns
		for (int col_i = -2; col_i <= 2; col_i++)
			for (int row_i = -1; row_i <= 1; row_i++)
				draw->AddCircleFilled(
					ImVec2(cx + col_i * STEP, cy + row_i * STEP),
					DOT_R, col);
	} else {
		// Five rows, two columns
		for (int row_i = -2; row_i <= 2; row_i++)
			for (int col_i = -1; col_i <= 1; col_i++)
				draw->AddCircleFilled(
					ImVec2(cx + col_i * STEP, cy + row_i * STEP),
					DOT_R, col);
	}
}

// ---- Main UI entry point --------------------------------------------------

void tools_draw_ui(struct escreen_state *state, cairo_t *cr) {
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)state->total_max_x, (float)state->total_max_y);
	ImGui::NewFrame();

	const bool pinned = state->sketching.toolbar_pinned;
	tool_interface_t *tool = (tool_interface_t*)state->sketching.active_tool;
	const bool has_options = tool->show_color || tool->show_thickness ||
	                          tool->show_hardness || tool->show_fill ||
	                          tool->type == TOOL_STAMP || tool->type == TOOL_TEXT;

	// Handle strip height; the strip is drawn as an overlay at the end of
	// the frame so it spans the final window width without contributing to
	// the measured content width (which would keep the window at an old,
	// wider size forever).
	const float HANDLE_H = 12.0f;
	auto draw_handle_spacer = [&]() {
		ImGui::Dummy(ImVec2(0.0f, HANDLE_H));
		ImGui::Spacing();
	};

	// ----------------------------------------------------------------
	// Tool icons
	// ----------------------------------------------------------------
	auto draw_icons = [&](bool vert) {
		if (vert) ImGui::BeginGroup();
		if (IconButton(state, "Select Area",       TOOL_SELECT,      tool == state->sketching.tools[TOOL_SELECT]))      tools_set_active(state, TOOL_SELECT);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Lasso Select Area",  TOOL_LASSO,       tool == state->sketching.tools[TOOL_LASSO]))       tools_set_active(state, TOOL_LASSO);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Brush Tool",         TOOL_BRUSH,       tool == state->sketching.tools[TOOL_BRUSH]))       tools_set_active(state, TOOL_BRUSH);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Blur Tool",          TOOL_BLUR,        tool == state->sketching.tools[TOOL_BLUR]))        tools_set_active(state, TOOL_BLUR);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Line Tool",          TOOL_LINE,        tool == state->sketching.tools[TOOL_LINE]))        tools_set_active(state, TOOL_LINE);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Rectangle Tool",     TOOL_RECTANGLE,   tool == state->sketching.tools[TOOL_RECTANGLE]))   tools_set_active(state, TOOL_RECTANGLE);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Arrow Tool",         TOOL_ARROW,       tool == state->sketching.tools[TOOL_ARROW]))       tools_set_active(state, TOOL_ARROW);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Stamp Tool",         TOOL_STAMP,       tool == state->sketching.tools[TOOL_STAMP]))       tools_set_active(state, TOOL_STAMP);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Text Tool",          TOOL_TEXT,        tool == state->sketching.tools[TOOL_TEXT]))        tools_set_active(state, TOOL_TEXT);
		if (!vert) ImGui::SameLine();
		if (IconButton(state, "Color Picker",       TOOL_COLORPICKER, tool == state->sketching.tools[TOOL_COLORPICKER])) tools_set_active(state, TOOL_COLORPICKER);
		if (vert) ImGui::EndGroup();
	};

	// ----------------------------------------------------------------
	// Tool options (colour swatch, sliders, etc.)
	// ----------------------------------------------------------------
	auto draw_options = [&](bool vert) {
		if (vert) ImGui::BeginGroup();

		// The option widgets keep their natural widths; the window is sized
		// from the actual layout, so each tool's panel is measured on the
		// fly rather than forced to a constant.
		const float wheel_w  = vert ? 160.0f : 240.0f;
		const float option_w = vert ? 130.0f : 120.0f;

		// Colour wheel embedded directly in the toolbar (no popup).
		if (tool->show_color) {
			float color[3] = {(float)state->sketching.r, (float)state->sketching.g, (float)state->sketching.b};
			ImGui::PushItemWidth(wheel_w);
			if (ImGui::ColorPicker3("##ColorWheel", color,
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview |
					ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoOptions |
					ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_PickerHueWheel)) {
				state->sketching.r = color[0];
				state->sketching.g = color[1];
				state->sketching.b = color[2];
			}
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color");
			ImGui::Spacing();
		}

		// Remaining options flow on a new row below the colour wheel.
		bool row_started = false;
		auto next = [&]() {
			if (!row_started) { row_started = true; return; }
			if (vert) ImGui::Spacing(); else ImGui::SameLine();
		};

		if (tool->show_thickness) {
			next();
			ImGui::PushItemWidth(option_w);
			float thickness = (float)state->sketching.thickness;
			if (ImGui::SliderFloat("##Size", &thickness, 1.0f, 100.0f, "Size: %.0f")) {
				state->sketching.thickness = thickness;
			}
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Size");
		}
		if (tool->show_hardness) {
			next();
			ImGui::PushItemWidth(option_w);
			float hardness = (float)state->sketching.hardness;
			if (ImGui::SliderFloat("##Hardness", &hardness, 0.0f, 1.0f, "Hard: %.2f")) {
				state->sketching.hardness = hardness;
			}
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hardness");
		}
		if (tool->show_fill) {
			next();
			ImGui::PushItemWidth(option_w);
			bool filled = state->sketching.filled;
			if (ImGui::Checkbox("Fill", &filled)) {
				state->sketching.filled = filled;
			}
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill");
		}
		if (tool->type == TOOL_STAMP) {
			next();
			int* counter_ptr = tool_stamp_get_counter_ptr();
			ImGui::PushItemWidth(vert ? 95 : 75);
			if (ImGui::InputInt("##StampCounter", counter_ptr)) {
				if (*counter_ptr < 1) *counter_ptr = 1;
			}
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next Number");
			if (ImGui::Button("Reset to 1")) {
				*counter_ptr = 1;
			}
		}
		if (vert) ImGui::EndGroup();
	};

	// The toolbar content as laid out inside a window. Whether the option
	// column comes before or after the icons does not change the size, so
	// this is used both for the off-screen measurement and the real window.
	auto draw_content = [&](bool vert) {
		draw_handle_spacer();
		draw_icons(vert);
		if (has_options) {
			if (vert) ImGui::SameLine(); else ImGui::Separator();
			draw_options(vert);
		}
	};

	// ----------------------------------------------------------------
	// Measure the exact content size for the active tool BEFORE the
	// toolbar is positioned. The content is submitted once into an
	// off-screen throwaway window per orientation; the resulting sizes are
	// used to place and size the real window in this same frame, so there
	// is no off-by-one frame when switching between tools.
	// ----------------------------------------------------------------
	ImVec2 size_v, size_h;
	{
		const ImGuiWindowFlags measure_flags =
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoBackground;
		const ImVec2 measure_pos(-100000.0f, -100000.0f);
		const ImVec2 measure_size(io.DisplaySize.x, io.DisplaySize.y);

		ImGui::SetNextWindowPos(measure_pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(measure_size, ImGuiCond_Always);
		ImGui::Begin("##ToolbarMeasureV", NULL, measure_flags);
		draw_content(true);
		ImGuiWindow *mv = ImGui::GetCurrentWindow();
		size_v = ImGui::CalcWindowNextAutoFitSize(mv);
		ImGui::End();

		ImGui::SetNextWindowPos(measure_pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(measure_size, ImGuiCond_Always);
		ImGui::Begin("##ToolbarMeasureH", NULL, measure_flags);
		draw_content(false);
		ImGuiWindow *mh = ImGui::GetCurrentWindow();
		size_h = ImGui::CalcWindowNextAutoFitSize(mh);
		ImGui::End();
	}

	// ----------------------------------------------------------------
	// Auto placement from the measured sizes (also sets is_vertical).
	// ----------------------------------------------------------------
	double auto_tx, auto_ty;
	get_toolbar_placement(state, size_h.x, size_h.y, size_v.x, size_v.y,
	                      &auto_tx, &auto_ty, &state->sketching.is_vertical);
	const bool vert = state->sketching.is_vertical;
	const ImVec2 tsize = vert ? size_v : size_h;
	const double auto_tw = tsize.x;

	static bool last_vert = state->sketching.is_vertical;
	if (last_vert != state->sketching.is_vertical) {
		state->sketching.ui_layout_frames = 12;
		last_vert = state->sketching.is_vertical;
	}

	float cx_eff = pinned ? (float)state->sketching.toolbar_pinned_x : (float)auto_tx;
	float cy_eff = pinned ? (float)state->sketching.toolbar_pinned_y : (float)auto_ty;

	// Clamp out-of-bounds using the measured window size
	{
		float center_x = cx_eff + tsize.x * 0.5f;
		float center_y = cy_eff + tsize.y * 0.5f;
		struct escreen_output *o, *best = NULL;
		wl_list_for_each(o, &state->outputs, link) {
			if (center_x >= o->logical_geometry.x && center_x < o->logical_geometry.x + o->logical_geometry.width &&
			    center_y >= o->logical_geometry.y && center_y < o->logical_geometry.y + o->logical_geometry.height) {
				best = o;
				break;
			}
		}

		float mon_min_x = best ? (float)best->logical_geometry.x : (float)state->total_min_x;
		float mon_min_y = best ? (float)best->logical_geometry.y : (float)state->total_min_y;
		float mon_max_x = best ? (float)(best->logical_geometry.x + best->logical_geometry.width)  : (float)state->total_max_x;
		float mon_max_y = best ? (float)(best->logical_geometry.y + best->logical_geometry.height) : (float)state->total_max_y;

		float min_x = mon_min_x + 4.0f;
		float max_x = mon_max_x - tsize.x - 4.0f;
		float min_y = mon_min_y + 4.0f;
		float max_y = mon_max_y - tsize.y - 4.0f;

		if (max_x < min_x) max_x = min_x; // Just in case window is wider than screen
		if (max_y < min_y) max_y = min_y;

		if (cx_eff < min_x) cx_eff = min_x;
		if (cx_eff > max_x) cx_eff = max_x;
		if (cy_eff < min_y) cy_eff = min_y;
		if (cy_eff > max_y) cy_eff = max_y;
	}

	if (pinned) {
		state->sketching.toolbar_pinned_x = cx_eff;
		state->sketching.toolbar_pinned_y = cy_eff;
	}

	ImGui::SetNextWindowSize(tsize, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(cx_eff, cy_eff), ImGuiCond_Always);

	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4((float)state->config.colors.toolbar_bg.r, (float)state->config.colors.toolbar_bg.g, (float)state->config.colors.toolbar_bg.b, (float)state->config.colors.toolbar_bg.a));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4((float)state->config.colors.button_hover.r, (float)state->config.colors.button_hover.g, (float)state->config.colors.button_hover.b, (float)state->config.colors.button_hover.a));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4((float)state->config.colors.accent.r, (float)state->config.colors.accent.g, (float)state->config.colors.accent.b, (float)state->config.colors.accent.a));
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4((float)state->config.colors.accent.r, (float)state->config.colors.accent.g, (float)state->config.colors.accent.b, (float)state->config.colors.accent.a));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4((float)state->config.colors.accent.r, (float)state->config.colors.accent.g, (float)state->config.colors.accent.b, (float)state->config.colors.accent.a));

	if (ImGui::Begin("Escreen Sketching", NULL,
	                 ImGuiWindowFlags_AlwaysAutoResize |
	                 ImGuiWindowFlags_NoCollapse       |
	                 ImGuiWindowFlags_NoTitleBar       |
	                 ImGuiWindowFlags_NoMove)) {

		// ----------------------------------------------------------------
		// Layout: icons + options, order determined by which side the
		// toolbar sits on (rev_x / rev_y mirror the order to keep options
		// closer to the selection).
		// When pinned, pivot is always top-left so we fall through to the
		// default (non-reversed) ordering.
		// ----------------------------------------------------------------
		{
			draw_handle_spacer();

			const bool rev_x = !pinned && (auto_tx + auto_tw / 2.0 < state->result.x);
			const bool rev_y = !pinned && (auto_ty > state->result.y + state->result.height - 10);

			if (vert) {
				if (rev_x && has_options) {
					draw_options(true);
					ImGui::SameLine();
					draw_icons(true);
				} else {
					draw_icons(true);
					if (has_options) { ImGui::SameLine(); draw_options(true); }
				}
			} else {
				if (rev_y && has_options) {
					draw_options(false);
					ImGui::Separator();
					draw_icons(false);
				} else {
					draw_icons(false);
					if (has_options) { ImGui::Separator(); draw_options(false); }
				}
			}
		}

		// ----------------------------------------------------------------
		// Drag handle overlay — drawn after the content so it can span the
		// final window width without influencing the measured layout.
		// ----------------------------------------------------------------
		{
			ImGuiWindow *win  = ImGui::GetCurrentWindow();
			ImDrawList *draw  = ImGui::GetWindowDrawList();

			ImRect handle_rect(win->InnerRect.Min,
			                   ImVec2(win->InnerRect.Min.x + win->InnerRect.GetWidth(),
			                          win->InnerRect.Min.y + HANDLE_H));

			ImGuiID id = win->GetID("##drag_handle");
			bool handle_hovered = false, handle_held = false;
			ImGui::ButtonBehavior(handle_rect, id, &handle_hovered, &handle_held);

			// Subtle background so the strip reads as a handle.
			if (handle_held || handle_hovered) {
				ImU32 bg = handle_held ? IM_COL32(255, 255, 255, 41)
				                       : IM_COL32(255, 255, 255, 20);
				draw->AddRectFilled(handle_rect.Min, handle_rect.Max, bg);
			}

			// On first press: snapshot where the window currently is.
			static ImVec2 drag_win_start;
			static bool   was_held = false;
			if (handle_held && !was_held) {
				drag_win_start = win->Pos;
				state->sketching.toolbar_pinned   = true;
				state->sketching.toolbar_pinned_x = drag_win_start.x;
				state->sketching.toolbar_pinned_y = drag_win_start.y;
			}
			was_held = handle_held;

			// While dragging: update stored position by accumulated delta.
			if (handle_held) {
				ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
				state->sketching.toolbar_pinned_x = drag_win_start.x + delta.x;
				state->sketching.toolbar_pinned_y = drag_win_start.y + delta.y;
			}

			// Double-click → unpin (return to auto-placement).
			if (handle_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				state->sketching.toolbar_pinned = false;
			}

			// Visual grip dots
			ImU32 grip_col = handle_held    ? IM_COL32(255, 255, 255, 210) :
			                 handle_hovered ? IM_COL32(255, 255, 255, 140) :
			                                  IM_COL32(200, 200, 200,  80);
			draw_grip_dots(draw, handle_rect.Min, handle_rect.Max,
			               /*horizontal=*/true, grip_col);

			if (handle_hovered)
				ImGui::SetTooltip("Drag to pin\nDouble-click to auto-place");
		}
	}
	ImGui::End();
	ImGui::PopStyleColor(5);

	// Remember the measured size for hit-testing outside this frame.
	if (vert) { g_toolbar_v_w = tsize.x; g_toolbar_v_h = tsize.y; }
	else      { g_toolbar_h_w = tsize.x; g_toolbar_h_h = tsize.y; }

	ImGui::Render();
	ImGui_ImplCairo_RenderDrawData(cr, ImGui::GetDrawData());
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
		state->sketching.ui_layout_frames = 12; // Force 12 frames to settle ImGui layout across all outputs
	}
}

void tools_handle_button(struct escreen_state *state, double x, double y, bool pressed) {
	ImGuiIO& io = ImGui::GetIO();
	io.MousePos = ImVec2((float)x, (float)y);
	io.MouseDown[0] = pressed;

	if (io.WantCaptureMouse) return;

	if (pressed) {
		state->sketching.drawing = true;
		if (state->sketching.active_tool && state->sketching.active_tool->on_mousedown) {
			state->sketching.active_tool->on_mousedown(state, x, y);
		}
	} else {
		if (state->sketching.drawing && state->sketching.active_tool && state->sketching.active_tool->on_mouseup) {
			state->sketching.active_tool->on_mouseup(state, x, y);
		}
		state->sketching.drawing = false;
	}
}

void tools_handle_motion(struct escreen_state *state, double x, double y) {
	ImGuiIO& io = ImGui::GetIO();
	io.MousePos = ImVec2((float)x, (float)y);

	if (state->sketching.drawing && state->sketching.active_tool && state->sketching.active_tool->on_mousemove) {
		state->sketching.active_tool->on_mousemove(state, x, y);
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

} // extern "C"
