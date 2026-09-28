#include "vitagl_bridge.h"

#if FLASHVITA_ENABLE_RUFFLE
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vitaGL.h>
#include "../platform/vita_native.h"

namespace {
bool g_flash_texture_enabled = false;
bool g_flash_texcoord_array = false;
bool g_flash_color_array = false;
bool g_flash_premultiplied_blend = false;
GLuint g_flash_bound_texture = 0;
int g_flash_filter = -1;
int g_flash_wrap = -1;
GLuint g_flash_offscreen_fbo = 0;
GLuint g_flash_offscreen_stencil = 0;
GLsizei g_flash_offscreen_width = 0;
GLsizei g_flash_offscreen_height = 0;
float g_flash_view_left = 0.0f;
float g_flash_view_top = 0.0f;
float g_flash_view_scale_x = 2.0f / 960.0f;
float g_flash_view_scale_y = 2.0f / 544.0f;

struct FlashGpuProgram {
    GLuint program = 0;
    GLint matrix0 = -1;
    GLint matrix1 = -1;
    GLint viewport = -1;
    GLint color_mult = -1;
    GLint color_add = -1;
    GLint sampler = -1;
};

FlashGpuProgram g_flash_gpu_color;
FlashGpuProgram g_flash_gpu_texture;
bool g_flash_gpu_programs_attempted = false;
bool g_flash_gpu_programs_ready = false;
bool g_flash_custom_program_active = false;
GLuint g_flash_current_program = 0;

void invalidateFlashGlCache();

struct FlashVitaGpuTransform {
    float a;
    float b;
    float c;
    float d;
    float tx;
    float ty;
    float mult[4];
    float add[4];
};

const char* kFlashColorVertexShader = R"(
uniform float4 uMatrix0;
uniform float4 uMatrix1;
uniform float4 uViewport;
uniform float4 uColorMult;
uniform float4 uColorAdd;

void main(
    float2 position,
    float4 color,
    float4 out outColor : COLOR0,
    float4 out gl_Position : POSITION
) {
    float2 stage;
    stage.x = position.x * uMatrix0.x + position.y * uMatrix0.y + uMatrix0.z;
    stage.y = position.x * uMatrix1.x + position.y * uMatrix1.y + uMatrix1.z;
    gl_Position = float4(
        (stage.x - uViewport.x) * uViewport.z - 1.0,
        1.0 - (stage.y - uViewport.y) * uViewport.w,
        0.0,
        1.0);
    float4 transformed = saturate(color * uColorMult + uColorAdd);
    outColor = color.a > 0.0 ? transformed : color;
}
)";

const char* kFlashColorFragmentShader = R"(
float4 main(float4 color : COLOR0) : COLOR {
    return color;
}
)";

const char* kFlashTextureVertexShader = R"(
uniform float4 uMatrix0;
uniform float4 uMatrix1;
uniform float4 uViewport;

void main(
    float2 position,
    float2 texCoord,
    float2 out outTexCoord : TEXCOORD0,
    float4 out gl_Position : POSITION
) {
    float2 stage;
    stage.x = position.x * uMatrix0.x + position.y * uMatrix0.y + uMatrix0.z;
    stage.y = position.x * uMatrix1.x + position.y * uMatrix1.y + uMatrix1.z;
    gl_Position = float4(
        (stage.x - uViewport.x) * uViewport.z - 1.0,
        1.0 - (stage.y - uViewport.y) * uViewport.w,
        0.0,
        1.0);
    outTexCoord = texCoord;
}
)";

const char* kFlashTextureFragmentShader = R"(
uniform sampler2D uTexture;
uniform float4 uColorMult;
uniform float4 uColorAdd;

float4 main(float2 texCoord : TEXCOORD0) : COLOR {
    float4 sampled = tex2D(uTexture, texCoord);
    if (sampled.a <= 0.0) return sampled;
    float4 straight = float4(sampled.rgb / sampled.a, sampled.a);
    float4 transformed = saturate(straight * uColorMult + uColorAdd);
    return float4(transformed.rgb * transformed.a, transformed.a);
}
)";

bool compileFlashGpuProgram(FlashGpuProgram& out,
                            const char* vertex_source,
                            const char* fragment_source,
                            bool textured) {
    GLuint vertex = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
    GLuint fragment = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
    if (!vertex || !fragment) return false;

    const GLint vertex_len = static_cast<GLint>(std::strlen(vertex_source));
    const GLint fragment_len = static_cast<GLint>(std::strlen(fragment_source));
    glShaderSource(vertex, 1, &vertex_source, &vertex_len);
    glCompileShader(vertex);
    GLint ok = GL_FALSE;
    glGetShaderiv(vertex, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    glShaderSource(fragment, 1, &fragment_source, &fragment_len);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }

    out.program = glCreateProgram();
    if (!out.program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    glAttachShader(out.program, vertex);
    glAttachShader(out.program, fragment);
    glBindAttribLocation(out.program, 0, "position");
    glBindAttribLocation(out.program, 1, textured ? "texCoord" : "color");
    glLinkProgram(out.program);
    glGetProgramiv(out.program, GL_LINK_STATUS, &ok);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!ok) {
        glDeleteProgram(out.program);
        out.program = 0;
        return false;
    }

    out.matrix0 = glGetUniformLocation(out.program, "uMatrix0");
    out.matrix1 = glGetUniformLocation(out.program, "uMatrix1");
    out.viewport = glGetUniformLocation(out.program, "uViewport");
    out.color_mult = glGetUniformLocation(out.program, "uColorMult");
    out.color_add = glGetUniformLocation(out.program, "uColorAdd");
    out.sampler = textured ? glGetUniformLocation(out.program, "uTexture") : -1;
    return true;
}

bool ensureFlashGpuPrograms() {
    if (g_flash_gpu_programs_attempted) return g_flash_gpu_programs_ready;
    g_flash_gpu_programs_attempted = true;
    g_flash_gpu_programs_ready =
        compileFlashGpuProgram(
            g_flash_gpu_color, kFlashColorVertexShader, kFlashColorFragmentShader, false) &&
        compileFlashGpuProgram(
            g_flash_gpu_texture, kFlashTextureVertexShader, kFlashTextureFragmentShader, true);
    return g_flash_gpu_programs_ready;
}

void useFfpProgram() {
    if (!g_flash_custom_program_active) return;
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    g_flash_custom_program_active = false;
    g_flash_current_program = 0;
    invalidateFlashGlCache();
}

void useFlashGpuProgram(const FlashGpuProgram& program) {
    if (g_flash_current_program != program.program) {
        glUseProgram(program.program);
        g_flash_current_program = program.program;
    }
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    g_flash_custom_program_active = true;
}

void setGpuTransform(const FlashGpuProgram& program, const FlashVitaGpuTransform& transform) {
    const GLfloat matrix0[4] = {transform.a, transform.c, transform.tx, 0.0f};
    const GLfloat matrix1[4] = {transform.b, transform.d, transform.ty, 0.0f};
    const GLfloat viewport[4] = {
        g_flash_view_left,
        g_flash_view_top,
        g_flash_view_scale_x,
        g_flash_view_scale_y,
    };
    glUniform4fv(program.matrix0, 1, matrix0);
    glUniform4fv(program.matrix1, 1, matrix1);
    glUniform4fv(program.viewport, 1, viewport);
    if (program.color_mult >= 0) glUniform4fv(program.color_mult, 1, transform.mult);
    if (program.color_add >= 0) glUniform4fv(program.color_add, 1, transform.add);
}

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

int32_t flashvita_vitagl_gpu_transform_available() {
    return ensureFlashGpuPrograms() ? 1 : 0;
}

uint64_t flashvita_vitagl_create_gpu_mesh(const void* vertices,
                                          size_t vertex_bytes,
                                          const uint16_t* indices,
                                          size_t index_count) {
    if (!vertices || vertex_bytes == 0 || !indices || index_count < 3 || !ensureFlashGpuPrograms()) {
        return 0;
    }

    GLuint vbo = 0;
    GLuint ibo = 0;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    if (!vbo || !ibo) {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ibo) glDeleteBuffers(1, &ibo);
        return 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(vertex_bytes), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizei>(index_count * sizeof(uint16_t)),
        indices,
        GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return (static_cast<uint64_t>(ibo) << 32) | static_cast<uint64_t>(vbo);
}

void flashvita_vitagl_delete_gpu_mesh(uint64_t handle) {
    if (!handle) return;
    useFfpProgram();
    GLuint vbo = static_cast<GLuint>(handle & 0xFFFFFFFFu);
    GLuint ibo = static_cast<GLuint>(handle >> 32);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ibo) glDeleteBuffers(1, &ibo);
}

void flashvita_vitagl_draw_gpu_colored(uint64_t handle,
                                       size_t index_count,
                                       const FlashVitaGpuTransform* transform) {
    if (!handle || index_count < 3 || !transform || !ensureFlashGpuPrograms()) return;
    const GLuint vbo = static_cast<GLuint>(handle & 0xFFFFFFFFu);
    const GLuint ibo = static_cast<GLuint>(handle >> 32);
    useFlashGpuProgram(g_flash_gpu_color);
    setGpuTransform(g_flash_gpu_color, *transform);

    if (g_flash_premultiplied_blend) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(FlashVitaRuffleVertex), nullptr);
    glVertexAttribPointer(
        1,
        4,
        GL_UNSIGNED_BYTE,
        GL_TRUE,
        sizeof(FlashVitaRuffleVertex),
        reinterpret_cast<const void*>(offsetof(FlashVitaRuffleVertex, r)));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, nullptr);
}

void flashvita_vitagl_draw_gpu_textured(uint64_t handle,
                                        size_t index_count,
                                        uint32_t texture,
                                        const FlashVitaGpuTransform* transform,
                                        uint8_t smoothing,
                                        uint8_t wrap_mode) {
    if (!handle || index_count < 3 || !texture || !transform || !ensureFlashGpuPrograms()) return;
    const GLuint vbo = static_cast<GLuint>(handle & 0xFFFFFFFFu);
    const GLuint ibo = static_cast<GLuint>(handle >> 32);
    useFlashGpuProgram(g_flash_gpu_texture);
    setGpuTransform(g_flash_gpu_texture, *transform);
    if (g_flash_gpu_texture.sampler >= 0) glUniform1i(g_flash_gpu_texture.sampler, 0);

    if (!g_flash_premultiplied_blend) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = true;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLint filter = smoothing ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    const GLint wrap = wrap_mode == 2 ? GL_MIRRORED_REPEAT
                                      : (wrap_mode == 1 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(FlashVitaRuffleTexVertex), nullptr);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(FlashVitaRuffleTexVertex),
        reinterpret_cast<const void*>(offsetof(FlashVitaRuffleTexVertex, u)));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, nullptr);
}

void flashvita_vitagl_invalidate_cache() {
    invalidateFlashGlCache();
}

void flashvita_vitagl_begin_flash_frame(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    useFfpProgram();
    g_flash_view_left = 0.0f;
    g_flash_view_top = 0.0f;
    g_flash_view_scale_x = 2.0f / 960.0f;
    g_flash_view_scale_y = 2.0f / 544.0f;
    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
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

int32_t flashvita_vitagl_begin_offscreen(uint32_t texture,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t x_min,
                                         uint32_t y_min,
                                         uint32_t x_max,
                                         uint32_t y_max,
                                         uint8_t r,
                                         uint8_t g,
                                         uint8_t b,
                                         uint8_t a) {
    if (!texture || width == 0 || height == 0 || x_max <= x_min || y_max <= y_min) return -1;
    useFfpProgram();
    g_flash_view_left = static_cast<float>(x_min);
    g_flash_view_top = static_cast<float>(y_min);
    g_flash_view_scale_x = 2.0f / static_cast<float>(x_max - x_min);
    g_flash_view_scale_y = 2.0f / static_cast<float>(y_max - y_min);

    if (!g_flash_offscreen_fbo) glGenFramebuffers(1, &g_flash_offscreen_fbo);
    if (!g_flash_offscreen_stencil) glGenRenderbuffers(1, &g_flash_offscreen_stencil);
    if (!g_flash_offscreen_fbo || !g_flash_offscreen_stencil) return -2;

    glBindFramebuffer(GL_FRAMEBUFFER, g_flash_offscreen_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, g_flash_offscreen_stencil);
    if (g_flash_offscreen_width != static_cast<GLsizei>(width) ||
        g_flash_offscreen_height != static_cast<GLsizei>(height)) {
        glRenderbufferStorage(
            GL_RENDERBUFFER, GL_STENCIL_INDEX8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        g_flash_offscreen_width = static_cast<GLsizei>(width);
        g_flash_offscreen_height = static_cast<GLsizei>(height);
    }
    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_flash_offscreen_stencil);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -3;
    }

    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(
        static_cast<GLdouble>(x_min),
        static_cast<GLdouble>(x_max),
        static_cast<GLdouble>(y_max),
        static_cast<GLdouble>(y_min),
        -1.0,
        1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
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
    return 0;
}

void flashvita_vitagl_end_offscreen() {
    useFfpProgram();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_flash_view_left = 0.0f;
    g_flash_view_top = 0.0f;
    g_flash_view_scale_x = 2.0f / 960.0f;
    g_flash_view_scale_y = 2.0f / 544.0f;
    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    invalidateFlashGlCache();
}

int32_t flashvita_vitagl_read_texture_region(uint32_t texture,
                                             uint32_t texture_width,
                                             uint32_t texture_height,
                                             uint32_t x,
                                             uint32_t y,
                                             uint32_t width,
                                             uint32_t height,
                                             uint8_t* destination) {
    if (!texture || !destination || width == 0 || height == 0 ||
        x + width > texture_width || y + height > texture_height) {
        return -1;
    }

    useFfpProgram();
    if (!g_flash_offscreen_fbo) glGenFramebuffers(1, &g_flash_offscreen_fbo);
    if (!g_flash_offscreen_fbo) return -2;

    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, g_flash_offscreen_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -3;
    }

    // GL's read origin is bottom-left, while Flash BitmapData is top-left.
    const GLint read_y = static_cast<GLint>(texture_height - y - height);
    glReadPixels(
        static_cast<GLint>(x),
        read_y,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        destination);

    const size_t row_bytes = static_cast<size_t>(width) * 4;
    for (uint32_t row = 0; row < height / 2; ++row) {
        uint8_t* top = destination + static_cast<size_t>(row) * row_bytes;
        uint8_t* bottom = destination + static_cast<size_t>(height - 1 - row) * row_bytes;
        for (size_t i = 0; i < row_bytes; ++i) {
            const uint8_t temp = top[i];
            top[i] = bottom[i];
            bottom[i] = temp;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    invalidateFlashGlCache();
    return 0;
}

void flashvita_vitagl_prepare_ui() {
    useFfpProgram();
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
    useFfpProgram();
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

void flashvita_vitagl_draw_colored_triangles_u16(const FlashVitaRuffleVertex* vertices,
                                                  size_t vertex_count,
                                                  const uint16_t* indices,
                                                  size_t index_count) {
    if (!vertices || !indices || vertex_count == 0 || index_count < 3) return;
    useFfpProgram();
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
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}

void flashvita_vitagl_draw_colored_line_strip(const FlashVitaRuffleVertex* vertices,
                                              size_t vertex_count) {
    if (!vertices || vertex_count < 2) return;
    useFfpProgram();
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

uint32_t flashvita_vitagl_create_texture_zero_copy(uint8_t* data, uint32_t width, uint32_t height) {
    if (!data || width == 0 || height == 0 || !flashvita_vita_vgl_ram_owns(data)) return 0;

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

    // Build a normal linear RGBA descriptor first, then replace its storage with
    // the BitmapData VGL_RAM block. The old storage is unused and can be released.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    void* old_data = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (old_data && old_data != data) vglFree(old_data);
    vglOverloadTexDataPointer(GL_TEXTURE_2D, data);
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
    // Texture storage is allocated by flashvita_vitagl_create_texture. Updating
    // the whole image must not reallocate it every time.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    GL_RGBA, GL_UNSIGNED_BYTE, data);
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

void flashvita_vitagl_delete_external_texture(uint32_t texture, const uint8_t* external_data) {
    if (!texture) return;
    glBindTexture(GL_TEXTURE_2D, texture);
    g_flash_bound_texture = texture;
    if (!external_data || vglGetTexDataPointer(GL_TEXTURE_2D) == external_data) {
        // The BitmapData owns this VGL_RAM allocation; prevent glDeleteTextures
        // from freeing it a second time.
        vglOverloadTexDataPointer(GL_TEXTURE_2D, nullptr);
    }
    const GLuint id = texture;
    glDeleteTextures(1, &id);
    g_flash_bound_texture = 0;
    g_flash_filter = -1;
    g_flash_wrap = -1;
}

void flashvita_vitagl_draw_textured_triangles(uint32_t texture,
                                              const FlashVitaRuffleTexVertex* vertices,
                                              size_t vertex_count,
                                              const uint32_t* indices,
                                              size_t index_count,
                                              uint8_t smoothing,
    uint8_t wrap_mode) {
    if (!texture || !vertices || !indices || vertex_count == 0 || index_count < 3) return;
    useFfpProgram();
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

void flashvita_vitagl_draw_textured_triangles_u16(uint32_t texture,
                                                  const FlashVitaRuffleTexVertex* vertices,
                                                  size_t vertex_count,
                                                  const uint16_t* indices,
                                                  size_t index_count,
                                                  uint8_t smoothing,
                                                  uint8_t wrap_mode) {
    if (!texture || !vertices || !indices || vertex_count == 0 || index_count < 3) return;
    useFfpProgram();
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
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}
} // extern "C"
#endif
