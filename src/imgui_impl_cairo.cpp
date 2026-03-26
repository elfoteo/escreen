#include "imgui.h"
#include <cairo.h>
#include <math.h>
#include <vector>
#include <stdint.h>

static cairo_surface_t* g_FontSurface = NULL;

void ImGui_ImplCairo_CreateFontsTexture() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    g_FontSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    unsigned char* dest = cairo_image_surface_get_data(g_FontSurface);
    
    for (int i = 0; i < width * height; i++) {
        uint8_t r = pixels[i * 4 + 0];
        uint8_t g = pixels[i * 4 + 1];
        uint8_t b = pixels[i * 4 + 2];
        uint8_t a = pixels[i * 4 + 3];
        // Premultiply and store as native endian ARGB32 (BGRA)
        r = (r * a) / 255;
        g = (g * a) / 255;
        b = (b * a) / 255;
        uint32_t val = (a << 24) | (r << 16) | (g << 8) | b;
        ((uint32_t*)dest)[i] = val;
    }
    cairo_surface_mark_dirty(g_FontSurface);
    io.Fonts->TexID = (ImTextureID)g_FontSurface;
}

void ImGui_ImplCairo_DestroyFontsTexture() {
    if (g_FontSurface) {
        cairo_surface_destroy(g_FontSurface);
        g_FontSurface = NULL;
    }
}

void ImGui_ImplCairo_RenderDrawData(cairo_t* cr, ImDrawData* draw_data) {
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buffer = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer = cmd_list->IdxBuffer.Data;

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
            } else {
                cairo_save(cr);
                cairo_rectangle(cr, pcmd->ClipRect.x, pcmd->ClipRect.y, pcmd->ClipRect.z - pcmd->ClipRect.x, pcmd->ClipRect.w - pcmd->ClipRect.y);
                cairo_clip(cr);

                for (unsigned int i = 0; i < pcmd->ElemCount; i += 3) {
                    ImDrawIdx i0 = idx_buffer[pcmd->IdxOffset + i];
                    ImDrawIdx i1 = idx_buffer[pcmd->IdxOffset + i + 1];
                    ImDrawIdx i2 = idx_buffer[pcmd->IdxOffset + i + 2];

                    const ImDrawVert& v0 = vtx_buffer[pcmd->VtxOffset + i0];
                    const ImDrawVert& v1 = vtx_buffer[pcmd->VtxOffset + i1];
                    const ImDrawVert& v2 = vtx_buffer[pcmd->VtxOffset + i2];

                    cairo_save(cr);
                    cairo_move_to(cr, v0.pos.x, v0.pos.y);
                    cairo_line_to(cr, v1.pos.x, v1.pos.y);
                    cairo_line_to(cr, v2.pos.x, v2.pos.y);
                    cairo_close_path(cr);
                    cairo_clip_preserve(cr);

                    if (pcmd->GetTexID()) {
                        cairo_surface_t* surface = (cairo_surface_t*)pcmd->GetTexID();
                        int width = cairo_image_surface_get_width(surface);
                        int height = cairo_image_surface_get_height(surface);

                        float u0 = v0.uv.x * width, v0p = v0.uv.y * height;
                        float u1 = v1.uv.x * width, v1p = v1.uv.y * height;
                        float u2 = v2.uv.x * width, v2p = v2.uv.y * height;

                        float det = (v1.pos.x - v0.pos.x) * (v2.pos.y - v0.pos.y) - (v2.pos.x - v0.pos.x) * (v1.pos.y - v0.pos.y);
                        if (fabs(det) > 1e-6) {
                            cairo_matrix_t matrix;
                            matrix.xx = ((u1 - u0) * (v2.pos.y - v0.pos.y) - (u2 - u0) * (v1.pos.y - v0.pos.y)) / det;
                            matrix.xy = ((v1.pos.x - v0.pos.x) * (u2 - u0) - (v2.pos.x - v0.pos.x) * (u1 - u0)) / det;
                            matrix.x0 = u0 - matrix.xx * v0.pos.x - matrix.xy * v0.pos.y;

                            matrix.yx = ((v1p - v0p) * (v2.pos.y - v0.pos.y) - (v2p - v0p) * (v1.pos.y - v0.pos.y)) / det;
                            matrix.yy = ((v1.pos.x - v0.pos.x) * (v2p - v0p) - (v2.pos.x - v0.pos.x) * (v1p - v0p)) / det;
                            matrix.y0 = v0p - matrix.yx * v0.pos.x - matrix.yy * v0.pos.y;

                            float matrix_det = matrix.xx * matrix.yy - matrix.xy * matrix.yx;
                            if (fabs(matrix_det) > 1e-6) {
                                cairo_pattern_t* pattern = cairo_pattern_create_for_surface(surface);
                                cairo_pattern_set_matrix(pattern, &matrix);
                                cairo_pattern_set_filter(pattern, CAIRO_FILTER_BILINEAR);
                                
                                float r = ((v0.col >> 0) & 0xFF) / 255.0f;
                                float g = ((v0.col >> 8) & 0xFF) / 255.0f;
                                float b = ((v0.col >> 16) & 0xFF) / 255.0f;
                                float a = ((v0.col >> 24) & 0xFF) / 255.0f;
                                
                                cairo_set_source_rgba(cr, r, g, b, a);
                                cairo_mask(cr, pattern);
                                cairo_pattern_destroy(pattern);
                                cairo_new_path(cr); 
                            } else {
                                if (v0.col == v1.col && v1.col == v2.col) {
                                    float r = ((v0.col >> 0) & 0xFF) / 255.0f;
                                    float g = ((v0.col >> 8) & 0xFF) / 255.0f;
                                    float b = ((v0.col >> 16) & 0xFF) / 255.0f;
                                    float a = ((v0.col >> 24) & 0xFF) / 255.0f;
                                    cairo_set_source_rgba(cr, r, g, b, a);
                                    cairo_fill(cr);
                                } else {
                                    cairo_pattern_t *mesh = cairo_pattern_create_mesh();
                                    cairo_mesh_pattern_begin_patch(mesh);
                                    cairo_mesh_pattern_move_to(mesh, v0.pos.x, v0.pos.y);
                                    cairo_mesh_pattern_line_to(mesh, v1.pos.x, v1.pos.y);
                                    cairo_mesh_pattern_line_to(mesh, v2.pos.x, v2.pos.y);
                                    cairo_mesh_pattern_line_to(mesh, v2.pos.x, v2.pos.y);

                                    auto set_col = [](cairo_pattern_t* m, int corner, ImU32 col) {
                                        float r = ((col >> 0) & 0xFF) / 255.0f;
                                        float g = ((col >> 8) & 0xFF) / 255.0f;
                                        float b = ((col >> 16) & 0xFF) / 255.0f;
                                        float a = ((col >> 24) & 0xFF) / 255.0f;
                                        cairo_mesh_pattern_set_corner_color_rgba(m, corner, r, g, b, a);
                                    };
                                    set_col(mesh, 0, v0.col);
                                    set_col(mesh, 1, v1.col);
                                    set_col(mesh, 2, v2.col);
                                    set_col(mesh, 3, v2.col);

                                    cairo_mesh_pattern_end_patch(mesh);

                                    cairo_set_source(cr, mesh);
                                    cairo_fill(cr);
                                    cairo_pattern_destroy(mesh);
                                }
                            }
                        }
                    } else {
                        float r = ((v0.col >> 0) & 0xFF) / 255.0f;
                        float g = ((v0.col >> 8) & 0xFF) / 255.0f;
                        float b = ((v0.col >> 16) & 0xFF) / 255.0f;
                        float a = ((v0.col >> 24) & 0xFF) / 255.0f;
                        cairo_set_source_rgba(cr, r, g, b, a);
                        cairo_fill(cr);
                    }
                    cairo_restore(cr);
                }
                cairo_restore(cr);
            }
        }
    }
}
