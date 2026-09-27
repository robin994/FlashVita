#include "vitagl_bridge.h"

#if FLASHVITA_ENABLE_RUFFLE
#include <cstddef>
#include <cstdint>
#include <vitaGL.h>

namespace {
bool g_flash_texture_enabled = false;
bool g_flash_texcoord_array = false;
bool g_flash_color_array = false;
bool g_flash_premultiplied_blend = false;
GLuint g_flash_bound_texture = 0;
int g_flash_filter = -1;
int g_flash_wrap = -1;

void invalidateFlashGlCache() {
    g_flash_texture_enabled = false;
    g_flash_texcoord_array = false;
    g_flash_color_array = false;
    g_flash_premultiplied_blend = false;
    g_flash_bound_texture = 0;
    g_flash_filter = -1;
    g_flash_wrap = -1;
}

} // namespace

extern "C" {
struct FlashVitaRuffleVertex {
    float x;
    float y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct FlashVitaRuffleTexVertex {
    float x;
    float y;
    float u;
    float v;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

void flashvita_vitagl_invalidate_cache() {
    invalidateFlashGlCache();
}

void flashvita_vitagl_begin_flash_frame(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    invalidateFlashGlCache();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void flashvita_vitagl_prepare_ui() {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    invalidateFlashGlCache();
}

void flashvita_vitagl_mask_push(uint32_t previous_depth) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(previous_depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
}

void flashvita_vitagl_mask_activate(uint32_t depth) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x00);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void flashvita_vitagl_mask_deactivate(uint32_t depth) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
}

void flashvita_vitagl_mask_pop(uint32_t depth) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (depth == 0) {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    } else {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x00);
        glStencilFunc(GL_EQUAL, static_cast<GLint>(depth), 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
}

void flashvita_vitagl_draw_colored_triangles(const FlashVitaRuffleVertex* vertices,
                                              size_t vertex_count,
                                              const uint32_t* indices,
                                              size_t index_count) {
    if (!vertices || !indices || vertex_count == 0 || index_count < 3) return;
    if (g_flash_premultiplied_blend) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = false;
    }

    if (g_flash_texture_enabled) {
        glDisable(GL_TEXTURE_2D);
        g_flash_texture_enabled = false;
    }
    if (g_flash_texcoord_array) {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        g_flash_texcoord_array = false;
    }
    if (!g_flash_color_array) {
        glEnableClientState(GL_COLOR_ARRAY);
        g_flash_color_array = true;
    }
    glVertexPointer(2, GL_FLOAT, sizeof(FlashVitaRuffleVertex), &vertices[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(FlashVitaRuffleVertex), &vertices[0].r);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, indices);
}

void flashvita_vitagl_draw_colored_line_strip(const FlashVitaRuffleVertex* vertices,
                                              size_t vertex_count) {
    if (!vertices || vertex_count < 2) return;
    if (g_flash_premultiplied_blend) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = false;
    }
    if (g_flash_texture_enabled) {
        glDisable(GL_TEXTURE_2D);
        g_flash_texture_enabled = false;
    }
    if (g_flash_texcoord_array) {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        g_flash_texcoord_array = false;
    }
    if (!g_flash_color_array) {
        glEnableClientState(GL_COLOR_ARRAY);
        g_flash_color_array = true;
    }
    glVertexPointer(2, GL_FLOAT, sizeof(FlashVitaRuffleVertex), &vertices[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(FlashVitaRuffleVertex), &vertices[0].r);
    glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(vertex_count));
}

uint32_t flashvita_vitagl_create_texture(const uint8_t* data, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    g_flash_bound_texture = texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_flash_filter = 1;
    g_flash_wrap = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

void flashvita_vitagl_update_texture(uint32_t texture, const uint8_t* data,
                                     uint32_t width, uint32_t height) {
    if (!texture || !data || width == 0 || height == 0) return;
    if (g_flash_bound_texture != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        g_flash_bound_texture = texture;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
    // Initialize empty textures on their first update and handle large dirty regions.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

void flashvita_vitagl_update_texture_region(uint32_t texture, const uint8_t* data,
                                            uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height) {
    if (!texture || !data || width == 0 || height == 0) return;
    if (g_flash_bound_texture != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        g_flash_bound_texture = texture;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(y),
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    GL_RGBA, GL_UNSIGNED_BYTE, data);
}

void flashvita_vitagl_delete_texture(uint32_t texture) {
    if (!texture) return;
    const GLuint id = texture;
    glDeleteTextures(1, &id);
    if (g_flash_bound_texture == texture) {
        g_flash_bound_texture = 0;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
}

void flashvita_vitagl_draw_textured_triangles(uint32_t texture,
                                              const FlashVitaRuffleTexVertex* vertices,
                                              size_t vertex_count,
                                              const uint32_t* indices,
                                              size_t index_count,
                                              uint8_t smoothing,
    uint8_t wrap_mode) {
    if (!texture || !vertices || !indices || vertex_count == 0 || index_count < 3) return;
    if (!g_flash_premultiplied_blend) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = true;
    }
    if (!g_flash_texture_enabled) {
        glEnable(GL_TEXTURE_2D);
        g_flash_texture_enabled = true;
    }
    if (g_flash_bound_texture != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        g_flash_bound_texture = texture;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
    const int filter = smoothing ? 1 : 0;
    if (g_flash_filter != filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smoothing ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smoothing ? GL_LINEAR : GL_NEAREST);
        g_flash_filter = filter;
    }
    const GLint wrap = wrap_mode == 2 ? GL_MIRRORED_REPEAT
                                      : (wrap_mode == 1 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (g_flash_wrap != static_cast<int>(wrap_mode)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        g_flash_wrap = static_cast<int>(wrap_mode);
    }
    if (!g_flash_texcoord_array) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        g_flash_texcoord_array = true;
    }
    if (!g_flash_color_array) {
        glEnableClientState(GL_COLOR_ARRAY);
        g_flash_color_array = true;
    }
    glVertexPointer(2, GL_FLOAT, sizeof(FlashVitaRuffleTexVertex), &vertices[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(FlashVitaRuffleTexVertex), &vertices[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(FlashVitaRuffleTexVertex), &vertices[0].r);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, indices);
}
} // extern "C"
#endif
