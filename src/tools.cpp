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
	ImGui::DestroyContext();
}

static void get_toolbar_rect(struct escreen_state *state, double *x, double *y, double *w, double *h) {
	*w = 200;
	*h = 420; 

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
	double tx, ty, tw, th;
	get_toolbar_rect(state, &tx, &ty, &tw, &th);
	return (x >= tx && x < tx + tw && y >= ty && y < ty + th);
}

void tools_draw_ui(struct escreen_state *state, cairo_t *cr) {
	ImGuiIO& io = ImGui::GetIO();
	double tx, ty, tw, th;
	get_toolbar_rect(state, &tx, &ty, &tw, &th);

	io.DisplaySize = ImVec2((float)state->total_max_x, (float)state->total_max_y);
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2((float)tx, (float)ty), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2((float)tw, (float)th), ImGuiCond_Always);

	if (ImGui::Begin("Escreen Sketching", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
		if (ImGui::Button("Select Area", ImVec2(-FLT_MIN, 0))) tools_set_active(state, TOOL_SELECT);
		if (ImGui::Button("Brush Tool", ImVec2(-FLT_MIN, 0))) tools_set_active(state, TOOL_BRUSH);
		if (ImGui::Button("Blur Brush", ImVec2(-FLT_MIN, 0))) tools_set_active(state, TOOL_BLUR);
		if (ImGui::Button("Line Tool", ImVec2(-FLT_MIN, 0))) tools_set_active(state, TOOL_LINE);
		if (ImGui::Button("Rect Tool", ImVec2(-FLT_MIN, 0))) tools_set_active(state, TOOL_RECTANGLE);

		ImGui::Separator();
		ImGui::Text("Properties");
		
		float color[3] = {(float)state->sketching.r, (float)state->sketching.g, (float)state->sketching.b};
		if (ImGui::ColorPicker3("Color", color, ImGuiColorEditFlags_NoSidePreview)) {
			state->sketching.r = color[0];
			state->sketching.g = color[1];
			state->sketching.b = color[2];
		}

		float thickness = (float)state->sketching.thickness;
		if (ImGui::SliderFloat("Size", &thickness, 1.0f, 100.0f)) {
			state->sketching.thickness = thickness;
		}

		if (state->sketching.active_tool == &tool_brush || state->sketching.active_tool == &tool_blur) {
			float hardness = (float)state->sketching.hardness;
			if (ImGui::SliderFloat("Hardness", &hardness, 0.0f, 1.0f)) {
				state->sketching.hardness = hardness;
			}
		}

		bool filled = state->sketching.filled;
		if (ImGui::Checkbox("Filled Shape", &filled)) {
			state->sketching.filled = filled;
		}
	}
	ImGui::End();

	ImGui::Render();
	ImGui_ImplCairo_RenderDrawData(cr, ImGui::GetDrawData());
}

extern "C" {

void tools_draw(struct escreen_state *state, cairo_t *cr) {
	// History is already baked into state->global_capture
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
	if (state->sketching.history_count >= state->sketching.history_capacity) {
		state->sketching.history_capacity *= 2;
		state->sketching.history = (action_t*)realloc(state->sketching.history, state->sketching.history_capacity * sizeof(action_t));
	}
	state->sketching.history[state->sketching.history_count++] = action;

	// Bake stroke into the global capture texture so we don't have to redraw history
	if (state->global_capture && cairo_surface_status(state->global_capture) == CAIRO_STATUS_SUCCESS) {
		int max_scale = 1;
		struct escreen_output *o;
		wl_list_for_each(o, &state->outputs, link) {
			if (o->scale > max_scale) max_scale = o->scale;
		}
		cairo_t *cr = cairo_create(state->global_capture);
		cairo_scale(cr, (double)max_scale, (double)max_scale);
		cairo_translate(cr, -state->total_min_x, -state->total_min_y);
		state->sketching.tools[action.type]->render_action(state, cr, &action);
		cairo_destroy(cr);
	}
}

} // extern "C"
