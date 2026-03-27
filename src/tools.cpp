#include "imgui.h"
#include "escreen.h"
#include "tools.h"
#include <vector>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>

extern void ImGui_ImplCairo_RenderDrawData(cairo_t* cr, ImDrawData* draw_data);
extern void ImGui_ImplCairo_CreateFontsTexture();
extern void ImGui_ImplCairo_DestroyFontsTexture();

// Forward declarations of tool interfaces
extern "C" tool_interface_t tool_brush;
extern "C" tool_interface_t tool_blur;
extern "C" tool_interface_t tool_line;
extern "C" tool_interface_t tool_rectangle;

void tools_init(struct escreen_state *state) {
	state->sketching.tools[TOOL_SELECT] = NULL;
	state->sketching.tools[TOOL_BRUSH] = &tool_brush;
	state->sketching.tools[TOOL_BLUR] = &tool_blur;
	state->sketching.tools[TOOL_LINE] = &tool_line;
	state->sketching.tools[TOOL_RECTANGLE] = &tool_rectangle;
	
	state->sketching.active_tool = state->sketching.tools[TOOL_SELECT];
	
	state->sketching.r = 1.0f; state->sketching.g = 0.0f; state->sketching.b = 0.0f; state->sketching.a = 1.0f;
	state->sketching.thickness = 5.0f;
	state->sketching.hardness = 0.5f;
	state->sketching.filled = false;
	
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
		free(state->sketching.history[i].points);
	}
	free(state->sketching.history);
	if (state->sketching.history_layer) {
		cairo_surface_destroy(state->sketching.history_layer);
		state->sketching.history_layer = NULL;
	}
	ImGui::DestroyContext();
}

static void get_toolbar_rect(struct escreen_state *state, double *x, double *y, double *w, double *h) {
	*w = 270;
	*h = 100; 

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

	double sx = state->result.x;
	double sy = state->result.y;
	double sw = state->result.width;
	double sh = state->result.height;

	auto check_placement = [&](double px, double py, double *out_x, double *out_y) -> bool {
		if (py < min_y + 4) py = min_y + 4;
		if (py + *h > max_y - 4) py = max_y - *h - 4;
		if (px < min_x + 4) px = min_x + 4;
		if (px + *w > max_x - 4) px = max_x - *w - 4;

		bool intersect_x = px < sx + sw && px + *w > sx;
		bool intersect_y = py < sy + sh && py + *h > sy;
		
		*out_x = px;
		*out_y = py;
		return !(intersect_x && intersect_y);
	};

	double px, py;
	if (check_placement(sx - *w - 12, sy + (sh - *h) / 2.0, &px, &py)) { *x = px; *y = py; return; }
	if (check_placement(sx + sw + 12, sy + (sh - *h) / 2.0, &px, &py)) { *x = px; *y = py; return; }
	if (check_placement(sx + (sw - *w) / 2.0, sy + sh + 12, &px, &py)) { *x = px; *y = py; return; }
	if (check_placement(sx + (sw - *w) / 2.0, sy - *h - 12, &px, &py)) { *x = px; *y = py; return; }

	// Fallback if it must intersect
	*x = sx + 12;
	*y = sy + 12;
	if (*x < min_x + 4) *x = min_x + 4;
	if (*x + *w > max_x - 4) *x = max_x - *w - 4;
	if (*y < min_y + 4) *y = min_y + 4;
	if (*y + *h > max_y - 4) *y = max_y - *h - 4;
}

bool tools_is_on_toolbar(struct escreen_state *state, double x, double y) {
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
		return true;
	}
	double tx, ty, tw, th;
	get_toolbar_rect(state, &tx, &ty, &tw, &th);
	return (x >= tx && x < tx + tw && y >= ty && y < ty + th);
}

static bool IconButton(const char* tooltip, tool_type_t type, bool is_active) {
    ImVec2 size(32, 32);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(tooltip, size);
    bool hovered = ImGui::IsItemHovered();
    
    if (hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }
    
    ImU32 bg_col = is_active ? IM_COL32(40, 100, 200, 255) : 
                   hovered ? IM_COL32(80, 80, 80, 255) : 
                   IM_COL32(40, 40, 40, 255);
    ImU32 fg_col = IM_COL32(230, 230, 230, 255);
    
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg_col, 4.0f);
    
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
        draw->AddRect(ImVec2(c.x - 10, c.y - 6), ImVec2(c.x + 10, c.y + 6), fg_col, 0, 0, 2.0f);
    }
    
    return clicked;
}

void tools_draw_ui(struct escreen_state *state, cairo_t *cr) {
	ImGuiIO& io = ImGui::GetIO();
	double tx, ty, tw, th;
	get_toolbar_rect(state, &tx, &ty, &tw, &th);

	io.DisplaySize = ImVec2((float)state->total_max_x, (float)state->total_max_y);
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2((float)tx, (float)ty), ImGuiCond_Always);

	if (ImGui::Begin("Escreen Sketching", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
		if (IconButton("Select Area", TOOL_SELECT, state->sketching.active_tool == state->sketching.tools[TOOL_SELECT])) tools_set_active(state, TOOL_SELECT);
		ImGui::SameLine();
		if (IconButton("Brush Tool", TOOL_BRUSH, state->sketching.active_tool == state->sketching.tools[TOOL_BRUSH])) tools_set_active(state, TOOL_BRUSH);
		ImGui::SameLine();
		if (IconButton("Blur Tool", TOOL_BLUR, state->sketching.active_tool == state->sketching.tools[TOOL_BLUR])) tools_set_active(state, TOOL_BLUR);
		ImGui::SameLine();
		if (IconButton("Line Tool", TOOL_LINE, state->sketching.active_tool == state->sketching.tools[TOOL_LINE])) tools_set_active(state, TOOL_LINE);
		ImGui::SameLine();
		if (IconButton("Rectangle Tool", TOOL_RECTANGLE, state->sketching.active_tool == state->sketching.tools[TOOL_RECTANGLE])) tools_set_active(state, TOOL_RECTANGLE);

		ImGui::Spacing();
		
		float color[3] = {(float)state->sketching.r, (float)state->sketching.g, (float)state->sketching.b};
		
		ImGui::PushItemWidth(100);
		if (ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
			state->sketching.r = color[0];
			state->sketching.g = color[1];
			state->sketching.b = color[2];
		}
		ImGui::PopItemWidth();
		
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color");

		ImGui::SameLine();
		
		ImGui::PushItemWidth(110);
		float thickness = (float)state->sketching.thickness;
		if (ImGui::SliderFloat("##Size", &thickness, 1.0f, 100.0f, "Size: %.0f")) {
			state->sketching.thickness = thickness;
		}
		ImGui::PopItemWidth();

		if (state->sketching.active_tool == state->sketching.tools[TOOL_BRUSH] || state->sketching.active_tool == state->sketching.tools[TOOL_BLUR]) {
			ImGui::SameLine();
			float hardness = (float)state->sketching.hardness;
			ImGui::PushItemWidth(90);
			if (ImGui::SliderFloat("##Hardness", &hardness, 0.0f, 1.0f, "Hard: %.2f")) {
				state->sketching.hardness = hardness;
			}
			ImGui::PopItemWidth();
		} else if (state->sketching.active_tool == state->sketching.tools[TOOL_RECTANGLE]) {
			ImGui::SameLine();
			bool filled = state->sketching.filled;
			if (ImGui::Checkbox("Fill", &filled)) {
				state->sketching.filled = filled;
			}
		}
	}
	ImGui::End();

	ImGui::Render();
	ImGui_ImplCairo_RenderDrawData(cr, ImGui::GetDrawData());
}

extern "C" {

void tools_draw(struct escreen_state *state, cairo_t *cr) {
	if (!state->sketching.history_layer && state->global_capture) {
		int w = cairo_image_surface_get_width(state->global_capture);
		int h = cairo_image_surface_get_height(state->global_capture);
		state->sketching.history_layer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	}
	
	if (state->sketching.history_layer) {
		if (state->sketching.history_rendered_count != state->sketching.history_undo_pos) {
			cairo_t *lcr = cairo_create(state->sketching.history_layer);
			int max_scale = 1;
			struct escreen_output *o;
			wl_list_for_each(o, &state->outputs, link) {
				if (o->scale > max_scale) max_scale = o->scale;
			}
			
			if (state->sketching.history_undo_pos < state->sketching.history_rendered_count) {
				cairo_set_operator(lcr, CAIRO_OPERATOR_CLEAR);
				cairo_paint(lcr);
				cairo_set_operator(lcr, CAIRO_OPERATOR_OVER);
				state->sketching.history_rendered_count = 0;
			}
			
			cairo_scale(lcr, (double)max_scale, (double)max_scale);
			cairo_translate(lcr, -state->total_min_x, -state->total_min_y);
			
			for (size_t i = state->sketching.history_rendered_count; i < state->sketching.history_undo_pos; i++) {
				action_t *action = &state->sketching.history[i];
				state->sketching.tools[action->type]->render_action(state, lcr, action);
			}
			cairo_destroy(lcr);
			state->sketching.history_rendered_count = state->sketching.history_undo_pos;
		}
	}

	if (state->sketching.drawing && state->sketching.active_tool) {
		state->sketching.active_tool->draw_preview(state, cr);
	}
}

void tools_set_active(struct escreen_state *state, tool_type_t type) {
	if (type < TOOL_COUNT) {
		state->sketching.active_tool = state->sketching.tools[type];
	}
}

void tools_handle_button(struct escreen_state *state, double x, double y, bool pressed) {
	ImGuiIO& io = ImGui::GetIO();
	io.MousePos = ImVec2((float)x, (float)y);
	io.MouseDown[0] = pressed;

	if (io.WantCaptureMouse) return;

	if (pressed) {
		state->sketching.drawing = true;
		if (state->sketching.active_tool) {
			state->sketching.active_tool->on_mousedown(state, x, y);
		}
	} else {
		if (state->sketching.drawing && state->sketching.active_tool) {
			state->sketching.active_tool->on_mouseup(state, x, y);
		}
		state->sketching.drawing = false;
	}
}

void tools_handle_motion(struct escreen_state *state, double x, double y) {
	ImGuiIO& io = ImGui::GetIO();
	io.MousePos = ImVec2((float)x, (float)y);

	if (state->sketching.drawing && state->sketching.active_tool) {
		state->sketching.active_tool->on_mousemove(state, x, y);
	}
}

void tools_add_action(struct escreen_state *state, action_t action) {
	for (size_t i = state->sketching.history_undo_pos; i < state->sketching.history_count; i++) {
		free(state->sketching.history[i].points);
	}
	state->sketching.history_count = state->sketching.history_undo_pos;

	if (state->sketching.history_count >= state->sketching.history_capacity) {
		state->sketching.history_capacity *= 2;
		state->sketching.history = (action_t*)realloc(state->sketching.history, state->sketching.history_capacity * sizeof(action_t));
	}
	state->sketching.history[state->sketching.history_count++] = action;
	state->sketching.history_undo_pos = state->sketching.history_count;
}

} // extern "C"
