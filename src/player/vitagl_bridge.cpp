#include "vitagl_bridge.h"

#if FLASHVITA_ENABLE_RUFFLE
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <new>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/gxm.h>
#include <vitashark.h>
#include <vitaGL.h>
#include "../platform/vita_native.h"

extern "C" SceGxmContext* gxm_context;
extern "C" SceGxmShaderPatcher* gxm_shader_patcher;
extern "C" void scene_reset(void);
extern "C" GLboolean is_shark_online;
extern "C" GLboolean start_shader_compiler(void);
extern "C" SceGxmRenderTarget* gxm_render_target;
extern "C" SceGxmColorSurface gxm_color_surfaces[];
extern "C" SceGxmDepthStencilSurface gxm_depth_stencil_surface;
extern "C" SceGxmSyncObject* gxm_sync_objects[];
extern "C" unsigned int gxm_back_buffer_index;

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

struct NativeGpuMesh {
    void* vertices = nullptr;
    void* indices = nullptr;
    size_t vertex_bytes = 0;
    size_t index_count = 0;
    SceGxmIndexFormat index_format = SCE_GXM_INDEX_FORMAT_U16;
    SceGxmPrimitiveType primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    GLuint fallback_vbo = 0;
    GLuint fallback_ibo = 0;
    bool native = false;
};

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

enum class RenderCommandType : uint8_t {
    BeginFrame,
    GpuColor,
    GpuTexture,
    MaskPush,
    MaskActivate,
    MaskDeactivate,
    MaskPop,
    Color32,
    Color16,
    ColorLine,
    Texture32,
    Texture16,
};

struct RenderCommand {
    RenderCommandType type = RenderCommandType::BeginFrame;
    uint64_t handle = 0;
    size_t index_count = 0;
    uint32_t texture = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint8_t smoothing = 0;
    uint8_t wrap_mode = 0;
    uint8_t clear[4]{};
    FlashVitaGpuTransform transform{};
};

struct RenderPacket {
    std::vector<RenderCommand> commands;
    std::vector<FlashVitaRuffleVertex> color_vertices;
    std::vector<FlashVitaRuffleTexVertex> tex_vertices;
    std::vector<uint16_t> indices16;
    std::vector<uint32_t> indices32;
    void* gpu_color_vertices = nullptr;
    void* gpu_tex_vertices = nullptr;
    uint16_t* gpu_indices16 = nullptr;
    uint32_t* gpu_indices32 = nullptr;
    size_t gpu_color_capacity = 0;
    size_t gpu_tex_capacity = 0;
    size_t gpu_indices16_capacity = 0;
    size_t gpu_indices32_capacity = 0;
    bool present = false;

    void reset() {
        commands.clear();
        color_vertices.clear();
        tex_vertices.clear();
        indices16.clear();
        indices32.clear();
        present = false;
    }
};

RenderPacket g_render_packets[2];
std::atomic<int32_t> g_render_packet_state[2]{}; // 0=free, 1=capturing, 2=queued/executing
SceUID g_render_thread = -1;
SceUID g_render_ready_sema = -1;
SceUID g_render_done_sema = -1;
std::atomic<int32_t> g_render_pending{0};
std::atomic<bool> g_render_thread_started{false};
std::atomic<bool> g_render_thread_stop{false};
std::atomic<bool> g_async_pipeline_enabled{true};
int g_render_write_index = 0;
int g_render_read_index = 0;
int g_capture_index = -1;

struct NativeDynamicScratch {
    void* color_vertices = nullptr;
    void* tex_vertices = nullptr;
    uint16_t* indices16 = nullptr;
    uint32_t* indices32 = nullptr;
    size_t color_capacity = 0;
    size_t tex_capacity = 0;
    size_t indices16_capacity = 0;
    size_t indices32_capacity = 0;
    std::vector<uint16_t> line_indices16;
    std::vector<uint32_t> line_indices32;
};

NativeDynamicScratch g_sync_scratch;

struct ZeroCopyTextureState {
    uint8_t* buffers[2]{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t front = 0;
};

struct NativeOffscreenTarget {
    SceGxmRenderTarget* render_target = nullptr;
    SceGxmColorSurface color_surface{};
    SceGxmDepthStencilSurface depth_stencil{};
    void* depth_buffer = nullptr;
    void* stencil_buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    void* color_data = nullptr;
};

std::unordered_map<GLuint, ZeroCopyTextureState> g_zero_copy_textures;
std::unordered_map<GLuint, SceGxmTexture*> g_native_texture_views;
std::unordered_map<GLuint, NativeOffscreenTarget> g_native_offscreen_targets;
GLuint g_native_offscreen_texture = 0;
std::atomic<uint64_t> g_async_frames_submitted{0};
std::atomic<uint64_t> g_async_frames_executed{0};
std::atomic<uint64_t> g_async_waits{0};
std::atomic<uint64_t> g_native_draws{0};
std::atomic<uint64_t> g_fallback_draws{0};
std::atomic<uint64_t> g_native_texture_updates{0};
std::atomic<uint64_t> g_gl_texture_updates{0};
std::atomic<uint64_t> g_native_mask_ops{0};
std::atomic<uint64_t> g_gl_mask_ops{0};
std::atomic<uint64_t> g_native_offscreen_passes{0};
std::atomic<uint64_t> g_gl_offscreen_passes{0};

bool ensureRenderThread();
bool onRenderThread();
void waitRenderIdleInternal();
bool beginCaptureFrame(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int32_t commitCaptureFrame(bool present);
void shutdownRenderThreadInternal();
bool captureCommand(const RenderCommand& command);
bool captureColorDraw(RenderCommand command,
                      const FlashVitaRuffleVertex* vertices,
                      size_t vertex_count,
                      const void* indices,
                      size_t index_count,
                      bool indices_u16);
bool captureLineDraw(RenderCommand command,
                     const FlashVitaRuffleVertex* vertices,
                     size_t vertex_count);
bool captureTextureDraw(RenderCommand command,
                        const FlashVitaRuffleTexVertex* vertices,
                        size_t vertex_count,
                        const void* indices,
                        size_t index_count,
                        bool indices_u16);
bool ensurePacketGpuStaging(RenderPacket& packet);
bool drawNativeDynamicColor(const FlashVitaRuffleVertex* vertices,
                            size_t vertex_count,
                            const void* indices,
                            size_t index_count,
                            SceGxmIndexFormat index_format,
                            SceGxmPrimitiveType primitive);
bool drawNativeDynamicTexture(uint32_t texture,
                              const FlashVitaRuffleTexVertex* vertices,
                              size_t vertex_count,
                              const void* indices,
                              size_t index_count,
                              SceGxmIndexFormat index_format,
                              uint8_t smoothing,
                              uint8_t wrap_mode);
bool drawNativeDynamicLine(const FlashVitaRuffleVertex* vertices, size_t vertex_count);
bool beginNativeOffscreen(uint32_t texture,
                          uint32_t width,
                          uint32_t height,
                          uint32_t x_min,
                          uint32_t y_min,
                          uint32_t x_max,
                          uint32_t y_max,
                          uint8_t r,
                          uint8_t g,
                          uint8_t b,
                          uint8_t a);
bool endNativeOffscreen();
void destroyNativeOffscreenTarget(uint32_t texture);

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
bool g_native_mask_write = false;

struct NativeGxmProgram {
    SceGxmProgram* vertex_binary = nullptr;
    SceGxmProgram* fragment_binary = nullptr;
    SceGxmShaderPatcherId vertex_id{};
    SceGxmShaderPatcherId fragment_id{};
    bool vertex_registered = false;
    bool fragment_registered = false;
    SceGxmVertexProgram* vertex_program = nullptr;
    SceGxmVertexProgram* vertex_program_u32 = nullptr;
    SceGxmFragmentProgram* fragment_program = nullptr;
    SceGxmFragmentProgram* mask_fragment_program = nullptr;
    SceGxmFragmentProgram* clear_fragment_program = nullptr;
    const SceGxmProgramParameter* matrix0 = nullptr;
    const SceGxmProgramParameter* matrix1 = nullptr;
    const SceGxmProgramParameter* viewport = nullptr;
    const SceGxmProgramParameter* color_mult = nullptr;
    const SceGxmProgramParameter* color_add = nullptr;
    bool ready = false;
};

NativeGxmProgram g_native_gxm_color;
NativeGxmProgram g_native_gxm_texture;
bool g_native_gxm_attempted = false;
bool g_native_gxm_ready = false;
bool g_native_gxm_color_ready = false;
bool g_native_gxm_texture_ready = false;
NativeGpuMesh g_native_clear_mesh;

bool setNativeUniforms(const NativeGxmProgram& program,
                       const FlashVitaGpuTransform& transform,
                       bool textured);
void setNativeStencilState(SceGxmStencilFunc func,
                           SceGxmStencilOp pass_op,
                           uint8_t reference,
                           uint8_t write_mask);
void resetNativeStencilState();
FlashVitaGpuTransform identityGpuTransform();

void nativeGxmLog(const char* label, const char* stage, int32_t result = 0) {
    char line[192];
    sceClibSnprintf(
        line,
        sizeof(line),
        "native_gxm label=%s stage=%s result=0x%08X thread=%d render_thread=%d\n",
        label ? label : "?",
        stage ? stage : "?",
        static_cast<unsigned>(result),
        static_cast<int>(sceKernelGetThreadId()),
        static_cast<int>(g_render_thread));
    flashvita_vita_log_line(line);
}

void nativeSharkLog(const char* message, shark_log_level level, int line_number) {
    if (!message) return;
    char line[512];
    sceClibSnprintf(
        line,
        sizeof(line),
        "native_shark level=%d line=%d msg=%s\n",
        static_cast<int>(level),
        line_number,
        message);
    flashvita_vita_log_line(line);
}

void invalidateFlashGlCache();

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
    float4 color,
    float2 out outTexCoord : TEXCOORD0,
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
    outTexCoord = texCoord;
    outColor = color;
}
)";

const char* kFlashTextureFragmentShader = R"(
uniform sampler2D uTexture;
uniform float4 uColorMult;
uniform float4 uColorAdd;

float4 main(float2 texCoord : TEXCOORD0, float4 color : COLOR0) : COLOR {
    float4 sampled = tex2D(uTexture, texCoord) * color;
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
    if (textured) glBindAttribLocation(out.program, 2, "color");
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
    glDisableVertexAttribArray(2);
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

void releaseNativeGxmProgram(NativeGxmProgram& program) {
    if (program.clear_fragment_program && gxm_shader_patcher) {
        sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, program.clear_fragment_program);
    }
    if (program.mask_fragment_program && gxm_shader_patcher) {
        sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, program.mask_fragment_program);
    }
    if (program.fragment_program && gxm_shader_patcher) {
        sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, program.fragment_program);
    }
    if (program.vertex_program && gxm_shader_patcher) {
        sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, program.vertex_program);
    }
    if (program.vertex_program_u32 && gxm_shader_patcher) {
        sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, program.vertex_program_u32);
    }
    if (program.fragment_registered && gxm_shader_patcher) {
        sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, program.fragment_id);
    }
    if (program.vertex_registered && gxm_shader_patcher) {
        sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, program.vertex_id);
    }
    std::free(program.vertex_binary);
    std::free(program.fragment_binary);
    program = NativeGxmProgram{};
}

SceGxmProgram* compileNativeShader(const char* source, shark_type type) {
    uint32_t size = 0;
    SceGxmProgram* temporary = shark_compile_shader_extended(
        source,
        &size,
        type,
        SHARK_OPT_FAST,
        SHARK_ENABLE,
        SHARK_ENABLE,
        SHARK_ENABLE);
    if (!temporary || size == 0) {
        shark_clear_output();
        return nullptr;
    }
    auto* copy = static_cast<SceGxmProgram*>(std::malloc(size));
    if (copy) std::memcpy(copy, temporary, size);
    shark_clear_output();
    return copy;
}

bool createNativeGxmProgram(NativeGxmProgram& out,
                            const char* label,
                            const char* vertex_source,
                            const char* fragment_source,
                            bool textured,
                            bool premultiplied) {
    if (!gxm_shader_patcher) {
        nativeGxmLog(label, "no_shader_patcher");
        return false;
    }

    out.vertex_binary = compileNativeShader(vertex_source, SHARK_VERTEX_SHADER);
    out.fragment_binary = compileNativeShader(fragment_source, SHARK_FRAGMENT_SHADER);
    if (!out.vertex_binary || !out.fragment_binary) {
        nativeGxmLog(
            label,
            !out.vertex_binary ? "vertex_compile_failed" : "fragment_compile_failed");
        releaseNativeGxmProgram(out);
        return false;
    }
    const int vertex_check = sceGxmProgramCheck(out.vertex_binary);
    if (vertex_check != SCE_OK) {
        nativeGxmLog(label, "vertex_program_check", vertex_check);
        releaseNativeGxmProgram(out);
        return false;
    }
    const int fragment_check = sceGxmProgramCheck(out.fragment_binary);
    if (fragment_check != SCE_OK) {
        nativeGxmLog(label, "fragment_program_check", fragment_check);
        releaseNativeGxmProgram(out);
        return false;
    }
    const int vertex_register = sceGxmShaderPatcherRegisterProgram(
        gxm_shader_patcher, out.vertex_binary, &out.vertex_id);
    if (vertex_register != SCE_OK) {
        nativeGxmLog(label, "vertex_register", vertex_register);
        releaseNativeGxmProgram(out);
        return false;
    }
    out.vertex_registered = true;
    const int fragment_register = sceGxmShaderPatcherRegisterProgram(
        gxm_shader_patcher, out.fragment_binary, &out.fragment_id);
    if (fragment_register != SCE_OK) {
        nativeGxmLog(label, "fragment_register", fragment_register);
        releaseNativeGxmProgram(out);
        return false;
    }
    out.fragment_registered = true;

    const SceGxmProgramParameter* position =
        sceGxmProgramFindParameterByName(out.vertex_binary, "position");
    const SceGxmProgramParameter* secondary = sceGxmProgramFindParameterByName(
        out.vertex_binary, textured ? "texCoord" : "color");
    const SceGxmProgramParameter* vertex_color = textured
        ? sceGxmProgramFindParameterByName(out.vertex_binary, "color")
        : nullptr;
    if (!position || !secondary || (textured && !vertex_color)) {
        nativeGxmLog(label, "missing_vertex_attribute");
        releaseNativeGxmProgram(out);
        return false;
    }

    SceGxmVertexAttribute attributes[3]{};
    attributes[0].streamIndex = 0;
    attributes[0].offset = 0;
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[0].componentCount = 2;
    attributes[0].regIndex = static_cast<uint16_t>(
        sceGxmProgramParameterGetResourceIndex(position));
    attributes[1].streamIndex = 0;
    attributes[1].offset = textured
        ? static_cast<uint16_t>(offsetof(FlashVitaRuffleTexVertex, u))
        : static_cast<uint16_t>(offsetof(FlashVitaRuffleVertex, r));
    attributes[1].format = textured
        ? SCE_GXM_ATTRIBUTE_FORMAT_F32
        : SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attributes[1].componentCount = textured ? 2 : 4;
    attributes[1].regIndex = static_cast<uint16_t>(
        sceGxmProgramParameterGetResourceIndex(secondary));
    if (textured) {
        attributes[2].streamIndex = 0;
        attributes[2].offset = static_cast<uint16_t>(offsetof(FlashVitaRuffleTexVertex, r));
        attributes[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
        attributes[2].componentCount = 4;
        attributes[2].regIndex = static_cast<uint16_t>(
            sceGxmProgramParameterGetResourceIndex(vertex_color));
    }

    SceGxmVertexStream stream{};
    stream.stride = textured
        ? static_cast<uint16_t>(sizeof(FlashVitaRuffleTexVertex))
        : static_cast<uint16_t>(sizeof(FlashVitaRuffleVertex));
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    const int create_vertex_u16 = sceGxmShaderPatcherCreateVertexProgram(
            gxm_shader_patcher,
            out.vertex_id,
            attributes,
            textured ? 3 : 2,
            &stream,
            1,
            &out.vertex_program);
    if (create_vertex_u16 != SCE_OK) {
        nativeGxmLog(label, "create_vertex_u16", create_vertex_u16);
        releaseNativeGxmProgram(out);
        return false;
    }
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_32BIT;
    const int create_vertex_u32 = sceGxmShaderPatcherCreateVertexProgram(
            gxm_shader_patcher,
            out.vertex_id,
            attributes,
            textured ? 3 : 2,
            &stream,
            1,
            &out.vertex_program_u32);
    if (create_vertex_u32 != SCE_OK) {
        // U32 is an optional fast path. U16 rendering must stay native even if
        // this vertex variant is unavailable on a given vitaGL/GXM build.
        out.vertex_program_u32 = nullptr;
        nativeGxmLog(label, "create_vertex_u32_optional", create_vertex_u32);
    }

    SceGxmBlendInfo blend{};
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = premultiplied
        ? SCE_GXM_BLEND_FACTOR_ONE
        : SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    const int create_fragment = sceGxmShaderPatcherCreateFragmentProgram(
            gxm_shader_patcher,
            out.fragment_id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE,
            &blend,
            out.vertex_binary,
            &out.fragment_program);
    if (create_fragment != SCE_OK) {
        nativeGxmLog(label, "create_fragment", create_fragment);
        releaseNativeGxmProgram(out);
        return false;
    }
    SceGxmBlendInfo clear_blend = blend;
    clear_blend.colorSrc = SCE_GXM_BLEND_FACTOR_ONE;
    clear_blend.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
    clear_blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    clear_blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    const int create_clear = sceGxmShaderPatcherCreateFragmentProgram(
            gxm_shader_patcher,
            out.fragment_id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE,
            &clear_blend,
            out.vertex_binary,
            &out.clear_fragment_program);
    if (create_clear != SCE_OK) {
        out.clear_fragment_program = nullptr;
        nativeGxmLog(label, "create_clear_optional", create_clear);
    }
    SceGxmBlendInfo mask_blend = blend;
    mask_blend.colorMask = 0;
    const int create_mask = sceGxmShaderPatcherCreateFragmentProgram(
            gxm_shader_patcher,
            out.fragment_id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE,
            &mask_blend,
            out.vertex_binary,
            &out.mask_fragment_program);
    if (create_mask != SCE_OK) {
        out.mask_fragment_program = nullptr;
        nativeGxmLog(label, "create_mask_optional", create_mask);
    }

    out.matrix0 = sceGxmProgramFindParameterByName(out.vertex_binary, "uMatrix0");
    out.matrix1 = sceGxmProgramFindParameterByName(out.vertex_binary, "uMatrix1");
    out.viewport = sceGxmProgramFindParameterByName(out.vertex_binary, "uViewport");
    const SceGxmProgram* color_program = textured ? out.fragment_binary : out.vertex_binary;
    out.color_mult = sceGxmProgramFindParameterByName(color_program, "uColorMult");
    out.color_add = sceGxmProgramFindParameterByName(color_program, "uColorAdd");
    out.ready = out.matrix0 && out.matrix1 && out.viewport && out.color_mult && out.color_add;
    if (!out.ready) {
        nativeGxmLog(label, "missing_uniform");
        releaseNativeGxmProgram(out);
        return false;
    }
    nativeGxmLog(label, "ready");
    return true;
}

bool ensureNativeGxmPrograms() {
    if (g_native_gxm_attempted) return g_native_gxm_ready;
    g_native_gxm_attempted = true;
    if (!is_shark_online && !start_shader_compiler()) {
        nativeGxmLog("backend", "shark_init_failed");
        return false;
    }
    nativeGxmLog("backend", "shark_online");
    shark_install_log_cb(nativeSharkLog);
    shark_set_warnings_level(SHARK_WARN_HIGH);
    g_native_gxm_color_ready =
        createNativeGxmProgram(
            g_native_gxm_color,
            "color",
            kFlashColorVertexShader,
            kFlashColorFragmentShader,
            false,
            false);
    g_native_gxm_texture_ready =
        createNativeGxmProgram(
            g_native_gxm_texture,
            "texture",
            kFlashTextureVertexShader,
            kFlashTextureFragmentShader,
            true,
            true);
    g_native_gxm_ready = g_native_gxm_color_ready || g_native_gxm_texture_ready;
    nativeGxmLog("backend", g_native_gxm_ready ? "ready" : "unavailable");
    return g_native_gxm_ready;
}

bool ensureNativeClearMesh() {
    if (g_native_clear_mesh.native && g_native_clear_mesh.vertices && g_native_clear_mesh.indices) {
        return true;
    }
    if (!ensureNativeGxmPrograms() || !g_native_gxm_color_ready ||
        !g_native_gxm_color.clear_fragment_program ||
        !g_native_gxm_color.mask_fragment_program) {
        return false;
    }

    constexpr FlashVitaRuffleVertex kVertices[4] = {
        {0.0f, 0.0f, 0, 0, 0, 0},
        {960.0f, 0.0f, 0, 0, 0, 0},
        {960.0f, 544.0f, 0, 0, 0, 0},
        {0.0f, 544.0f, 0, 0, 0, 0},
    };
    constexpr uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};
    g_native_clear_mesh.vertices = vglAlloc(sizeof(kVertices), VGL_MEM_RAM);
    g_native_clear_mesh.indices = vglAlloc(sizeof(kIndices), VGL_MEM_RAM);
    if (!g_native_clear_mesh.vertices || !g_native_clear_mesh.indices) {
        if (g_native_clear_mesh.vertices) vglFree(g_native_clear_mesh.vertices);
        if (g_native_clear_mesh.indices) vglFree(g_native_clear_mesh.indices);
        g_native_clear_mesh = NativeGpuMesh{};
        return false;
    }
    if (sceDmacMemcpy(
            g_native_clear_mesh.vertices, kVertices, static_cast<SceSize>(sizeof(kVertices))) < 0) {
        std::memcpy(g_native_clear_mesh.vertices, kVertices, sizeof(kVertices));
    }
    if (sceDmacMemcpy(
            g_native_clear_mesh.indices, kIndices, static_cast<SceSize>(sizeof(kIndices))) < 0) {
        std::memcpy(g_native_clear_mesh.indices, kIndices, sizeof(kIndices));
    }
    g_native_clear_mesh.vertex_bytes = sizeof(kVertices);
    g_native_clear_mesh.index_count = 6;
    g_native_clear_mesh.index_format = SCE_GXM_INDEX_FORMAT_U16;
    g_native_clear_mesh.primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    g_native_clear_mesh.native = true;
    return true;
}

bool drawNativeClearCurrentScene(float left,
                                 float top,
                                 float right,
                                 float bottom,
                                 uint8_t r,
                                 uint8_t g,
                                 uint8_t b,
                                 uint8_t a) {
    if (!ensureNativeClearMesh() || !gxm_context) return false;
    if (!(right > left) || !(bottom > top)) return false;

    FlashVitaRuffleVertex vertices[4] = {
        {left, top, r, g, b, a},
        {right, top, r, g, b, a},
        {right, bottom, r, g, b, a},
        {left, bottom, r, g, b, a},
    };
    if (sceDmacMemcpy(
            g_native_clear_mesh.vertices, vertices, static_cast<SceSize>(sizeof(vertices))) < 0) {
        std::memcpy(g_native_clear_mesh.vertices, vertices, sizeof(vertices));
    }

    g_flash_view_left = left;
    g_flash_view_top = top;
    g_flash_view_scale_x = 2.0f / (right - left);
    g_flash_view_scale_y = 2.0f / (bottom - top);
    sceGxmSetCullMode(gxm_context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    resetNativeStencilState();

    const FlashVitaGpuTransform identity = identityGpuTransform();
    const NativeGxmProgram& program = g_native_gxm_color;
    sceGxmSetVertexProgram(gxm_context, program.vertex_program);
    sceGxmSetFragmentProgram(gxm_context, program.clear_fragment_program);
    if (!setNativeUniforms(program, identity, false)) return false;
    sceGxmSetVertexStream(gxm_context, 0, g_native_clear_mesh.vertices);
    if (sceGxmDraw(
            gxm_context,
            SCE_GXM_PRIMITIVE_TRIANGLES,
            SCE_GXM_INDEX_FORMAT_U16,
            g_native_clear_mesh.indices,
            6) != SCE_OK) {
        return false;
    }
    g_native_draws.fetch_add(1, std::memory_order_relaxed);

    setNativeStencilState(
        SCE_GXM_STENCIL_FUNC_ALWAYS,
        SCE_GXM_STENCIL_OP_REPLACE,
        0,
        0xFF);
    sceGxmSetFragmentProgram(gxm_context, program.mask_fragment_program);
    if (!setNativeUniforms(program, identity, false)) return false;
    if (sceGxmDraw(
            gxm_context,
            SCE_GXM_PRIMITIVE_TRIANGLES,
            SCE_GXM_INDEX_FORMAT_U16,
            g_native_clear_mesh.indices,
            6) != SCE_OK) {
        return false;
    }
    g_native_draws.fetch_add(1, std::memory_order_relaxed);
    resetNativeStencilState();
    g_native_mask_write = false;
    invalidateFlashGlCache();
    return true;
}

bool clearNativeSurface(float left,
                        float top,
                        float right,
                        float bottom,
                        uint8_t r,
                        uint8_t g,
                        uint8_t b,
                        uint8_t a) {
    scene_reset();
    return drawNativeClearCurrentScene(left, top, right, bottom, r, g, b, a);
}

bool clearNativeFrame(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return clearNativeSurface(0.0f, 0.0f, 960.0f, 544.0f, r, g, b, a);
}

bool nativeColorFormatForTexture(SceGxmTextureFormat texture_format,
                                 SceGxmColorFormat& color_format) {
    switch (texture_format) {
        case SCE_GXM_TEXTURE_FORMAT_U8_R:
            color_format = SCE_GXM_COLOR_FORMAT_U8_R;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U8U8_GR:
            color_format = SCE_GXM_COLOR_FORMAT_U8U8_GR;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR:
            color_format = SCE_GXM_COLOR_FORMAT_U8U8U8_BGR;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR:
            color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB:
            color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB:
            color_format = SCE_GXM_COLOR_FORMAT_U5U6U5_RGB;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ABGR:
            color_format = SCE_GXM_COLOR_FORMAT_U4U4U4U4_ABGR;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA:
            color_format = SCE_GXM_COLOR_FORMAT_U4U4U4U4_RGBA;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ABGR:
            color_format = SCE_GXM_COLOR_FORMAT_U1U5U5U5_ABGR;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA:
            color_format = SCE_GXM_COLOR_FORMAT_U5U5U5U1_RGBA;
            return true;
        case SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA:
            color_format = SCE_GXM_COLOR_FORMAT_F16F16F16F16_RGBA;
            return true;
        default:
            return false;
    }
}

uint32_t alignUpU32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

NativeOffscreenTarget* getNativeOffscreenTarget(uint32_t texture,
                                                 uint32_t width,
                                                 uint32_t height) {
    auto texture_it = g_native_texture_views.find(texture);
    if (texture_it == g_native_texture_views.end() || !texture_it->second) return nullptr;
    SceGxmTexture* native_texture = texture_it->second;
    const SceGxmTextureType texture_type = sceGxmTextureGetType(native_texture);
    if (texture_type != SCE_GXM_TEXTURE_LINEAR &&
        texture_type != SCE_GXM_TEXTURE_LINEAR_STRIDED) {
        return nullptr;
    }

    SceGxmColorFormat color_format{};
    if (!nativeColorFormatForTexture(sceGxmTextureGetFormat(native_texture), color_format)) {
        return nullptr;
    }
    void* color_data = sceGxmTextureGetData(native_texture);
    if (!color_data) return nullptr;

    auto existing = g_native_offscreen_targets.find(texture);
    if (existing != g_native_offscreen_targets.end()) {
        auto& target = existing->second;
        if (target.width == width && target.height == height && target.render_target) {
            if (target.color_data != color_data) {
                sceGxmColorSurfaceSetData(&target.color_surface, color_data);
                target.color_data = color_data;
            }
            return &target;
        }
        destroyNativeOffscreenTarget(texture);
    }

    NativeOffscreenTarget target{};
    target.width = width;
    target.height = height;
    target.color_data = color_data;

    SceGxmRenderTargetParams params{};
    params.width = width ? width : 1;
    params.height = height ? height : 1;
    params.scenesPerFrame = 1;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    if (sceGxmCreateRenderTarget(&params, &target.render_target) != SCE_OK ||
        !target.render_target) {
        return nullptr;
    }

    const uint32_t depth_width = alignUpU32(width, SCE_GXM_TILE_SIZEX);
    const uint32_t depth_height = alignUpU32(height, SCE_GXM_TILE_SIZEY);
    const size_t samples = static_cast<size_t>(depth_width) * depth_height;
    if (samples > static_cast<size_t>(UINT32_MAX) / 4u) {
        sceGxmDestroyRenderTarget(target.render_target);
        return nullptr;
    }
    target.depth_buffer = vglMemalign(
        SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT,
        static_cast<uint32_t>(samples * 4u));
    target.stencil_buffer = vglMemalign(
        SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT,
        static_cast<uint32_t>(samples));
    if (!target.depth_buffer || !target.stencil_buffer) {
        if (target.depth_buffer) vglFree(target.depth_buffer);
        if (target.stencil_buffer) vglFree(target.stencil_buffer);
        sceGxmDestroyRenderTarget(target.render_target);
        return nullptr;
    }

    if (sceGxmDepthStencilSurfaceInit(
            &target.depth_stencil,
            SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8,
            SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
            depth_width,
            target.depth_buffer,
            target.stencil_buffer) != SCE_OK) {
        vglFree(target.depth_buffer);
        vglFree(target.stencil_buffer);
        sceGxmDestroyRenderTarget(target.render_target);
        return nullptr;
    }

    uint32_t stride_pixels = alignUpU32(width, 8u);
    if (texture_type == SCE_GXM_TEXTURE_LINEAR_STRIDED) {
        const uint32_t stride_bytes = sceGxmTextureGetStride(native_texture);
        if (stride_bytes >= 4u) stride_pixels = stride_bytes / 4u;
    }
    const SceGxmOutputRegisterSize output_size =
        sceGxmTextureGetFormat(native_texture) == SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA
            ? SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT
            : SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
    if (sceGxmColorSurfaceInit(
            &target.color_surface,
            color_format,
            SCE_GXM_COLOR_SURFACE_LINEAR,
            SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            output_size,
            width,
            height,
            stride_pixels,
            color_data) != SCE_OK) {
        vglFree(target.depth_buffer);
        vglFree(target.stencil_buffer);
        sceGxmDestroyRenderTarget(target.render_target);
        return nullptr;
    }

    auto [inserted, ok] = g_native_offscreen_targets.emplace(texture, target);
    return ok ? &inserted->second : nullptr;
}

bool beginNativeOffscreen(uint32_t texture,
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
    if (g_native_offscreen_texture != 0 || !gxm_context) return false;
    NativeOffscreenTarget* target = getNativeOffscreenTarget(texture, width, height);
    if (!target) return false;

    scene_reset();
    if (sceGxmEndScene(gxm_context, nullptr, nullptr) != SCE_OK) return false;
    if (sceGxmBeginScene(
            gxm_context,
            0,
            target->render_target,
            nullptr,
            nullptr,
            nullptr,
            &target->color_surface,
            &target->depth_stencil) != SCE_OK) {
        sceGxmBeginScene(
            gxm_context,
            0,
            gxm_render_target,
            nullptr,
            nullptr,
            gxm_sync_objects[gxm_back_buffer_index],
            &gxm_color_surfaces[gxm_back_buffer_index],
            &gxm_depth_stencil_surface);
        return false;
    }

    g_native_offscreen_texture = texture;
    if (!drawNativeClearCurrentScene(
            static_cast<float>(x_min),
            static_cast<float>(y_min),
            static_cast<float>(x_max),
            static_cast<float>(y_max),
            r,
            g,
            b,
            a)) {
        endNativeOffscreen();
        return false;
    }
    return true;
}

bool endNativeOffscreen() {
    if (g_native_offscreen_texture == 0 || !gxm_context) return false;
    sceGxmEndScene(gxm_context, nullptr, nullptr);
    const int begin_result = sceGxmBeginScene(
        gxm_context,
        0,
        gxm_render_target,
        nullptr,
        nullptr,
        gxm_sync_objects[gxm_back_buffer_index],
        &gxm_color_surfaces[gxm_back_buffer_index],
        &gxm_depth_stencil_surface);
    g_native_offscreen_texture = 0;
    g_flash_view_left = 0.0f;
    g_flash_view_top = 0.0f;
    g_flash_view_scale_x = 2.0f / 960.0f;
    g_flash_view_scale_y = 2.0f / 544.0f;
    g_native_mask_write = false;
    resetNativeStencilState();
    invalidateFlashGlCache();
    return begin_result == SCE_OK;
}

void destroyNativeOffscreenTarget(uint32_t texture) {
    auto it = g_native_offscreen_targets.find(texture);
    if (it == g_native_offscreen_targets.end()) return;
    auto& target = it->second;
    if (target.render_target) sceGxmDestroyRenderTarget(target.render_target);
    if (target.depth_buffer) vglFree(target.depth_buffer);
    if (target.stencil_buffer) vglFree(target.stencil_buffer);
    g_native_offscreen_targets.erase(it);
}

bool setNativeUniforms(const NativeGxmProgram& program,
                       const FlashVitaGpuTransform& transform,
                       bool textured) {
    if (!gxm_context || !program.ready) return false;
    void* vertex_uniforms = nullptr;
    if (sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &vertex_uniforms) != SCE_OK ||
        !vertex_uniforms) {
        return false;
    }
    const float matrix0[4] = {transform.a, transform.c, transform.tx, 0.0f};
    const float matrix1[4] = {transform.b, transform.d, transform.ty, 0.0f};
    const float viewport[4] = {
        g_flash_view_left,
        g_flash_view_top,
        g_flash_view_scale_x,
        g_flash_view_scale_y,
    };
    sceGxmSetUniformDataF(vertex_uniforms, program.matrix0, 0, 4, matrix0);
    sceGxmSetUniformDataF(vertex_uniforms, program.matrix1, 0, 4, matrix1);
    sceGxmSetUniformDataF(vertex_uniforms, program.viewport, 0, 4, viewport);
    if (!textured) {
        sceGxmSetUniformDataF(vertex_uniforms, program.color_mult, 0, 4, transform.mult);
        sceGxmSetUniformDataF(vertex_uniforms, program.color_add, 0, 4, transform.add);
        return true;
    }

    void* fragment_uniforms = nullptr;
    if (sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &fragment_uniforms) != SCE_OK ||
        !fragment_uniforms) {
        return false;
    }
    sceGxmSetUniformDataF(fragment_uniforms, program.color_mult, 0, 4, transform.mult);
    sceGxmSetUniformDataF(fragment_uniforms, program.color_add, 0, 4, transform.add);
    return true;
}

bool drawNativeGxmMesh(NativeGpuMesh* mesh,
                       size_t index_count,
                       uint32_t texture,
                       const FlashVitaGpuTransform& transform,
                       uint8_t smoothing,
                       uint8_t wrap_mode) {
    if (!mesh || !mesh->native || !mesh->vertices || !mesh->indices ||
        !ensureNativeGxmPrograms()) {
        return false;
    }
    const bool textured = texture != 0;
    if ((textured && !g_native_gxm_texture_ready) ||
        (!textured && !g_native_gxm_color_ready)) {
        return false;
    }
    const NativeGxmProgram& program = textured ? g_native_gxm_texture : g_native_gxm_color;
    const SceGxmVertexProgram* vertex_program = mesh->index_format == SCE_GXM_INDEX_FORMAT_U32
        ? program.vertex_program_u32
        : program.vertex_program;
    if (!vertex_program) return false;
    sceGxmSetVertexProgram(gxm_context, vertex_program);
    SceGxmFragmentProgram* fragment_program =
        g_native_mask_write ? program.mask_fragment_program : program.fragment_program;
    if (!fragment_program) return false;
    sceGxmSetFragmentProgram(gxm_context, fragment_program);
    if (!setNativeUniforms(program, transform, textured)) return false;

    if (textured) {
        auto it = g_native_texture_views.find(texture);
        SceGxmTexture* gxm_texture = it != g_native_texture_views.end() ? it->second : nullptr;
        if (!gxm_texture) return false;
        const SceGxmTextureFilter filter = smoothing
            ? SCE_GXM_TEXTURE_FILTER_LINEAR
            : SCE_GXM_TEXTURE_FILTER_POINT;
        sceGxmTextureSetMinFilter(gxm_texture, filter);
        sceGxmTextureSetMagFilter(gxm_texture, filter);
        const SceGxmTextureAddrMode mode = wrap_mode == 2
            ? SCE_GXM_TEXTURE_ADDR_MIRROR
            : (wrap_mode == 1 ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetUAddrMode(gxm_texture, mode);
        sceGxmTextureSetVAddrMode(gxm_texture, mode);
        sceGxmSetFragmentTexture(gxm_context, 0, gxm_texture);
    }

    sceGxmSetVertexStream(gxm_context, 0, mesh->vertices);
    const size_t draw_count = std::min(index_count, mesh->index_count);
    if (draw_count < 3) return false;
    const int draw_result = sceGxmDraw(
        gxm_context,
        mesh->primitive,
        mesh->index_format,
        mesh->indices,
        static_cast<uint32_t>(draw_count));
    if (draw_result == SCE_OK) {
        g_native_draws.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void setNativeStencilState(SceGxmStencilFunc func,
                           SceGxmStencilOp pass_op,
                           uint8_t reference,
                           uint8_t write_mask) {
    if (!gxm_context) return;
    sceGxmSetTwoSidedEnable(gxm_context, SCE_GXM_TWO_SIDED_ENABLED);
    sceGxmSetFrontStencilRef(gxm_context, reference);
    sceGxmSetBackStencilRef(gxm_context, reference);
    sceGxmSetFrontStencilFunc(
        gxm_context,
        func,
        SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP,
        pass_op,
        0xFF,
        write_mask);
    sceGxmSetBackStencilFunc(
        gxm_context,
        func,
        SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP,
        pass_op,
        0xFF,
        write_mask);
}

void resetNativeStencilState() {
    setNativeStencilState(
        SCE_GXM_STENCIL_FUNC_ALWAYS,
        SCE_GXM_STENCIL_OP_KEEP,
        0,
        0);
}

bool updateNativeTextureMemory(uint32_t texture,
                               const uint8_t* data,
                               uint32_t x,
                               uint32_t y,
                               uint32_t width,
                               uint32_t height) {
    if (!data || width == 0 || height == 0) return false;
    const auto it = g_native_texture_views.find(texture);
    if (it == g_native_texture_views.end() || !it->second) return false;
    SceGxmTexture* native = it->second;
    const SceGxmTextureType type = sceGxmTextureGetType(native);
    if (type != SCE_GXM_TEXTURE_LINEAR && type != SCE_GXM_TEXTURE_LINEAR_STRIDED) {
        return false;
    }
    const uint32_t texture_width = sceGxmTextureGetWidth(native);
    const uint32_t texture_height = sceGxmTextureGetHeight(native);
    if (x + width > texture_width || y + height > texture_height) return false;
    auto* destination = static_cast<uint8_t*>(sceGxmTextureGetData(native));
    if (!destination) return false;
    size_t destination_stride = type == SCE_GXM_TEXTURE_LINEAR_STRIDED
        ? static_cast<size_t>(sceGxmTextureGetStride(native))
        : static_cast<size_t>(texture_width) * 4;
    if (destination_stride < static_cast<size_t>(texture_width) * 4) return false;

    const size_t source_stride = static_cast<size_t>(width) * 4;
    const size_t row_bytes = source_stride;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = data + static_cast<size_t>(row) * source_stride;
        uint8_t* dst = destination + static_cast<size_t>(y + row) * destination_stride +
            static_cast<size_t>(x) * 4;
        if (sceDmacMemcpy(dst, src, static_cast<SceSize>(row_bytes)) < 0) {
            std::memcpy(dst, src, row_bytes);
        }
    }
    g_native_texture_updates.fetch_add(1, std::memory_order_relaxed);
    return true;
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
int32_t flashvita_vitagl_gpu_transform_available() {
    waitRenderIdleInternal();
    return (ensureNativeGxmPrograms() || ensureFlashGpuPrograms()) ? 1 : 0;
}

uint64_t flashvita_vitagl_create_gpu_mesh(const void* vertices,
                                          size_t vertex_bytes,
                                          const uint16_t* indices,
                                          size_t index_count) {
    waitRenderIdleInternal();
    if (!vertices || vertex_bytes == 0 || !indices || index_count < 3) {
        return 0;
    }

    auto* mesh = new (std::nothrow) NativeGpuMesh();
    if (!mesh) return 0;
    mesh->vertex_bytes = vertex_bytes;
    mesh->index_count = index_count;
    mesh->index_format = SCE_GXM_INDEX_FORMAT_U16;
    mesh->primitive = SCE_GXM_PRIMITIVE_TRIANGLES;

    if (ensureNativeGxmPrograms() &&
        vertex_bytes <= static_cast<size_t>(UINT32_MAX) &&
        index_count <= static_cast<size_t>(UINT32_MAX / sizeof(uint16_t))) {
        mesh->vertices = vglAlloc(static_cast<uint32_t>(vertex_bytes), VGL_MEM_RAM);
        mesh->indices = vglAlloc(
            static_cast<uint32_t>(index_count * sizeof(uint16_t)), VGL_MEM_RAM);
        if (mesh->vertices && mesh->indices) {
            if (sceDmacMemcpy(mesh->vertices, vertices, static_cast<SceSize>(vertex_bytes)) < 0) {
                std::memcpy(mesh->vertices, vertices, vertex_bytes);
            }
            const size_t index_bytes = index_count * sizeof(uint16_t);
            if (sceDmacMemcpy(mesh->indices, indices, static_cast<SceSize>(index_bytes)) < 0) {
                std::memcpy(mesh->indices, indices, index_bytes);
            }
            mesh->native = true;
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mesh));
        }
        if (mesh->vertices) vglFree(mesh->vertices);
        if (mesh->indices) vglFree(mesh->indices);
        mesh->vertices = nullptr;
        mesh->indices = nullptr;
    }

    if (!ensureFlashGpuPrograms()) {
        delete mesh;
        return 0;
    }
    glGenBuffers(1, &mesh->fallback_vbo);
    glGenBuffers(1, &mesh->fallback_ibo);
    if (!mesh->fallback_vbo || !mesh->fallback_ibo) {
        if (mesh->fallback_vbo) glDeleteBuffers(1, &mesh->fallback_vbo);
        if (mesh->fallback_ibo) glDeleteBuffers(1, &mesh->fallback_ibo);
        delete mesh;
        return 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh->fallback_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(vertex_bytes), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->fallback_ibo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizei>(index_count * sizeof(uint16_t)),
        indices,
        GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mesh));
}

void flashvita_vitagl_delete_gpu_mesh(uint64_t handle) {
    if (!handle) return;
    waitRenderIdleInternal();
    auto* mesh = reinterpret_cast<NativeGpuMesh*>(static_cast<uintptr_t>(handle));
    if (!mesh) return;
    if (mesh->native) {
        if (mesh->vertices) vglFree(mesh->vertices);
        if (mesh->indices) vglFree(mesh->indices);
    } else {
        useFfpProgram();
        if (mesh->fallback_vbo) glDeleteBuffers(1, &mesh->fallback_vbo);
        if (mesh->fallback_ibo) glDeleteBuffers(1, &mesh->fallback_ibo);
    }
    delete mesh;
}

void flashvita_vitagl_draw_gpu_colored(uint64_t handle,
                                       size_t index_count,
                                       const FlashVitaGpuTransform* transform) {
    if (!handle || index_count < 3 || !transform) return;
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::GpuColor;
        command.handle = handle;
        command.index_count = index_count;
        command.transform = *transform;
        captureCommand(command);
        return;
    }
    auto* mesh = reinterpret_cast<NativeGpuMesh*>(static_cast<uintptr_t>(handle));
    if (drawNativeGxmMesh(mesh, index_count, 0, *transform, 0, 0)) return;
    if (!mesh || !mesh->fallback_vbo || !mesh->fallback_ibo || !ensureFlashGpuPrograms()) return;
    useFlashGpuProgram(g_flash_gpu_color);
    glDisableVertexAttribArray(2);
    setGpuTransform(g_flash_gpu_color, *transform);

    if (g_flash_premultiplied_blend) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_flash_premultiplied_blend = false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh->fallback_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->fallback_ibo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(FlashVitaRuffleVertex), nullptr);
    glVertexAttribPointer(
        1,
        4,
        GL_UNSIGNED_BYTE,
        GL_TRUE,
        sizeof(FlashVitaRuffleVertex),
        reinterpret_cast<const void*>(offsetof(FlashVitaRuffleVertex, r)));
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, nullptr);
}

void flashvita_vitagl_draw_gpu_textured(uint64_t handle,
                                        size_t index_count,
                                        uint32_t texture,
                                        const FlashVitaGpuTransform* transform,
                                        uint8_t smoothing,
                                        uint8_t wrap_mode) {
    if (!handle || index_count < 3 || !texture || !transform) return;
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::GpuTexture;
        command.handle = handle;
        command.index_count = index_count;
        command.texture = texture;
        command.transform = *transform;
        command.smoothing = smoothing;
        command.wrap_mode = wrap_mode;
        captureCommand(command);
        return;
    }
    auto* mesh = reinterpret_cast<NativeGpuMesh*>(static_cast<uintptr_t>(handle));
    if (drawNativeGxmMesh(mesh, index_count, texture, *transform, smoothing, wrap_mode)) return;
    if (!mesh || !mesh->fallback_vbo || !mesh->fallback_ibo || !ensureFlashGpuPrograms()) return;
    useFlashGpuProgram(g_flash_gpu_texture);
    glEnableVertexAttribArray(2);
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

    glBindBuffer(GL_ARRAY_BUFFER, mesh->fallback_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->fallback_ibo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(FlashVitaRuffleTexVertex), nullptr);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(FlashVitaRuffleTexVertex),
        reinterpret_cast<const void*>(offsetof(FlashVitaRuffleTexVertex, u)));
    glVertexAttribPointer(
        2,
        4,
        GL_UNSIGNED_BYTE,
        GL_TRUE,
        sizeof(FlashVitaRuffleTexVertex),
        reinterpret_cast<const void*>(offsetof(FlashVitaRuffleTexVertex, r)));
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, nullptr);
}

void flashvita_vitagl_invalidate_cache() {
    waitRenderIdleInternal();
    invalidateFlashGlCache();
}

void flashvita_vitagl_begin_flash_frame(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!onRenderThread() && beginCaptureFrame(r, g, b, a)) {
        return;
    }
    if (clearNativeFrame(r, g, b, a)) {
        return;
    }
    useFfpProgram();
    g_native_mask_write = false;
    if (ensureNativeGxmPrograms()) {
        resetNativeStencilState();
    }
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
    waitRenderIdleInternal();
    if (!texture || width == 0 || height == 0 || x_max <= x_min || y_max <= y_min) return -1;
    if (beginNativeOffscreen(
            texture, width, height, x_min, y_min, x_max, y_max, r, g, b, a)) {
        g_native_offscreen_passes.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
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
    g_gl_offscreen_passes.fetch_add(1, std::memory_order_relaxed);

    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    if (clearNativeSurface(
            static_cast<float>(x_min),
            static_cast<float>(y_min),
            static_cast<float>(x_max),
            static_cast<float>(y_max),
            r,
            g,
            b,
            a)) {
        return 0;
    }
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
    waitRenderIdleInternal();
    if (g_native_offscreen_texture != 0) {
        endNativeOffscreen();
        return;
    }
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
    waitRenderIdleInternal();
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
    waitRenderIdleInternal();
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
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::MaskPush;
        command.arg0 = previous_depth;
        captureCommand(command);
        return;
    }
    g_native_mask_write = true;
    if (ensureNativeGxmPrograms()) {
        g_native_mask_ops.fetch_add(1, std::memory_order_relaxed);
        setNativeStencilState(
            SCE_GXM_STENCIL_FUNC_EQUAL,
            SCE_GXM_STENCIL_OP_INCR,
            static_cast<uint8_t>(previous_depth),
            0xFF);
        return;
    }
    g_gl_mask_ops.fetch_add(1, std::memory_order_relaxed);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(previous_depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
}

void flashvita_vitagl_mask_activate(uint32_t depth) {
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::MaskActivate;
        command.arg0 = depth;
        captureCommand(command);
        return;
    }
    g_native_mask_write = false;
    if (ensureNativeGxmPrograms()) {
        g_native_mask_ops.fetch_add(1, std::memory_order_relaxed);
        setNativeStencilState(
            SCE_GXM_STENCIL_FUNC_EQUAL,
            SCE_GXM_STENCIL_OP_KEEP,
            static_cast<uint8_t>(depth),
            0x00);
        return;
    }
    g_gl_mask_ops.fetch_add(1, std::memory_order_relaxed);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x00);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void flashvita_vitagl_mask_deactivate(uint32_t depth) {
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::MaskDeactivate;
        command.arg0 = depth;
        captureCommand(command);
        return;
    }
    g_native_mask_write = true;
    if (ensureNativeGxmPrograms()) {
        g_native_mask_ops.fetch_add(1, std::memory_order_relaxed);
        setNativeStencilState(
            SCE_GXM_STENCIL_FUNC_EQUAL,
            SCE_GXM_STENCIL_OP_DECR,
            static_cast<uint8_t>(depth),
            0xFF);
        return;
    }
    g_gl_mask_ops.fetch_add(1, std::memory_order_relaxed);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, static_cast<GLint>(depth), 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
}

void flashvita_vitagl_mask_pop(uint32_t depth) {
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::MaskPop;
        command.arg0 = depth;
        captureCommand(command);
        return;
    }
    g_native_mask_write = false;
    if (ensureNativeGxmPrograms()) {
        g_native_mask_ops.fetch_add(1, std::memory_order_relaxed);
        if (depth == 0) {
            resetNativeStencilState();
        } else {
            setNativeStencilState(
                SCE_GXM_STENCIL_FUNC_EQUAL,
                SCE_GXM_STENCIL_OP_KEEP,
                static_cast<uint8_t>(depth),
                0x00);
        }
        return;
    }
    g_gl_mask_ops.fetch_add(1, std::memory_order_relaxed);
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
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::Color32;
        if (captureColorDraw(command, vertices, vertex_count, indices, index_count, false)) return;
    }
    if (drawNativeDynamicColor(
            vertices,
            vertex_count,
            indices,
            index_count,
            SCE_GXM_INDEX_FORMAT_U32,
            SCE_GXM_PRIMITIVE_TRIANGLES)) {
        return;
    }
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
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, indices);
}

void flashvita_vitagl_draw_colored_triangles_u16(const FlashVitaRuffleVertex* vertices,
                                                  size_t vertex_count,
                                                  const uint16_t* indices,
                                                  size_t index_count) {
    if (!vertices || !indices || vertex_count == 0 || index_count < 3) return;
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::Color16;
        if (captureColorDraw(command, vertices, vertex_count, indices, index_count, true)) return;
    }
    if (drawNativeDynamicColor(
            vertices,
            vertex_count,
            indices,
            index_count,
            SCE_GXM_INDEX_FORMAT_U16,
            SCE_GXM_PRIMITIVE_TRIANGLES)) {
        return;
    }
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
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}

void flashvita_vitagl_draw_colored_line_strip(const FlashVitaRuffleVertex* vertices,
                                              size_t vertex_count) {
    if (!vertices || vertex_count < 2) return;
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::ColorLine;
        if (captureLineDraw(command, vertices, vertex_count)) return;
    }
    if (drawNativeDynamicLine(vertices, vertex_count)) return;
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
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(vertex_count));
}

uint32_t flashvita_vitagl_create_texture(const uint8_t* data, uint32_t width, uint32_t height) {
    waitRenderIdleInternal();
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
    if (SceGxmTexture* native = vglGetGxmTexture(GL_TEXTURE_2D)) {
        g_native_texture_views[texture] = native;
    }
    return texture;
}

uint32_t flashvita_vitagl_create_texture_zero_copy(uint8_t* data, uint32_t width, uint32_t height) {
    waitRenderIdleInternal();
    if (!data || width == 0 || height == 0 || !flashvita_vita_vgl_ram_owns(data)) return 0;

    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (bytes == 0 || bytes > static_cast<size_t>(UINT32_MAX)) return 0;
    auto* front = static_cast<uint8_t*>(vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_RAM));
    auto* back = static_cast<uint8_t*>(vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_RAM));
    if (!front || !back) {
        if (front) vglFree(front);
        if (back) vglFree(back);
        return 0;
    }
    if (sceDmacMemcpy(front, data, static_cast<SceSize>(bytes)) < 0 ||
        sceDmacMemcpy(back, data, static_cast<SceSize>(bytes)) < 0) {
        std::memcpy(front, data, bytes);
        std::memcpy(back, data, bytes);
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        vglFree(front);
        vglFree(back);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    g_flash_bound_texture = texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_flash_filter = 1;
    g_flash_wrap = 0;

    // Build a normal linear RGBA descriptor first, then replace its storage with
    // the front VGL_RAM buffer. The CPU-owned BitmapData never becomes the live
    // GPU source, so CPU1 can safely sample while AVM mutates the source buffer.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        vglFree(front);
        vglFree(back);
        return 0;
    }
    void* old_data = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (old_data) vglFree(old_data);
    vglOverloadTexDataPointer(GL_TEXTURE_2D, front);
    if (SceGxmTexture* native = vglGetGxmTexture(GL_TEXTURE_2D)) {
        g_native_texture_views[texture] = native;
    }
    ZeroCopyTextureState state{};
    state.buffers[0] = front;
    state.buffers[1] = back;
    state.width = width;
    state.height = height;
    state.front = 0;
    g_zero_copy_textures.emplace(texture, state);
    return texture;
}

void flashvita_vitagl_update_zero_copy_texture(uint32_t texture,
                                               const uint8_t* data,
                                               uint32_t source_width,
                                               uint32_t source_height,
                                               uint32_t x,
                                               uint32_t y,
                                               uint32_t width,
                                               uint32_t height) {
    auto it = g_zero_copy_textures.find(texture);
    if (it == g_zero_copy_textures.end() || !data || width == 0 || height == 0) return;
    auto& state = it->second;
    if (source_width != state.width || source_height != state.height ||
        x + width > state.width || y + height > state.height) {
        return;
    }

    const uint8_t back_index = static_cast<uint8_t>(state.front ^ 1u);
    uint8_t* back = state.buffers[back_index];
    uint8_t* old_front = state.buffers[state.front];
    const size_t source_stride = static_cast<size_t>(source_width) * 4;
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = data + static_cast<size_t>(y + row) * source_stride +
            static_cast<size_t>(x) * 4;
        uint8_t* dst = back + static_cast<size_t>(y + row) * source_stride +
            static_cast<size_t>(x) * 4;
        if (sceDmacMemcpy(dst, src, static_cast<SceSize>(row_bytes)) < 0) {
            std::memcpy(dst, src, row_bytes);
        }
    }

    // Only the descriptor swap needs to wait for the previous GPU frame. The
    // dirty-region DMA above can overlap CPU0 logic with CPU1 render work.
    waitRenderIdleInternal();
    auto native_it = g_native_texture_views.find(texture);
    if (native_it != g_native_texture_views.end() && native_it->second) {
        sceGxmTextureSetData(native_it->second, back);
        g_native_texture_updates.fetch_add(1, std::memory_order_relaxed);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture);
        g_flash_bound_texture = texture;
        vglOverloadTexDataPointer(GL_TEXTURE_2D, back);
        g_gl_texture_updates.fetch_add(1, std::memory_order_relaxed);
    }
    state.front = back_index;

    // Keep the now-inactive buffer coherent so the next update only needs the
    // new dirty region rather than a full-frame copy.
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = data + static_cast<size_t>(y + row) * source_stride +
            static_cast<size_t>(x) * 4;
        uint8_t* dst = old_front + static_cast<size_t>(y + row) * source_stride +
            static_cast<size_t>(x) * 4;
        if (sceDmacMemcpy(dst, src, static_cast<SceSize>(row_bytes)) < 0) {
            std::memcpy(dst, src, row_bytes);
        }
    }
}

void flashvita_vitagl_update_texture(uint32_t texture, const uint8_t* data,
                                     uint32_t width, uint32_t height) {
    waitRenderIdleInternal();
    if (!texture || !data || width == 0 || height == 0) return;
    if (updateNativeTextureMemory(texture, data, 0, 0, width, height)) return;
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
    g_gl_texture_updates.fetch_add(1, std::memory_order_relaxed);
}

void flashvita_vitagl_update_texture_region(uint32_t texture, const uint8_t* data,
                                            uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height) {
    waitRenderIdleInternal();
    if (!texture || !data || width == 0 || height == 0) return;
    if (updateNativeTextureMemory(texture, data, x, y, width, height)) return;
    if (g_flash_bound_texture != texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        g_flash_bound_texture = texture;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(y),
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    GL_RGBA, GL_UNSIGNED_BYTE, data);
    g_gl_texture_updates.fetch_add(1, std::memory_order_relaxed);
}

void flashvita_vitagl_delete_texture(uint32_t texture) {
    if (!texture) return;
    waitRenderIdleInternal();
    destroyNativeOffscreenTarget(texture);
    const GLuint id = texture;
    g_native_texture_views.erase(texture);
    glDeleteTextures(1, &id);
    if (g_flash_bound_texture == texture) {
        g_flash_bound_texture = 0;
        g_flash_filter = -1;
        g_flash_wrap = -1;
    }
}

void flashvita_vitagl_delete_external_texture(uint32_t texture, const uint8_t* external_data) {
    if (!texture) return;
    waitRenderIdleInternal();
    destroyNativeOffscreenTarget(texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_flash_bound_texture = texture;
    auto it = g_zero_copy_textures.find(texture);
    if (it != g_zero_copy_textures.end()) {
        // Both mapped buffers are owned by this bridge; detach the active one so
        // glDeleteTextures cannot free it behind our back.
        vglOverloadTexDataPointer(GL_TEXTURE_2D, nullptr);
        for (auto* buffer : it->second.buffers) {
            if (buffer) vglFree(buffer);
        }
        g_zero_copy_textures.erase(it);
    } else if (!external_data || vglGetTexDataPointer(GL_TEXTURE_2D) == external_data) {
        vglOverloadTexDataPointer(GL_TEXTURE_2D, nullptr);
    }
    const GLuint id = texture;
    g_native_texture_views.erase(texture);
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
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::Texture32;
        command.texture = texture;
        command.smoothing = smoothing;
        command.wrap_mode = wrap_mode;
        if (captureTextureDraw(command, vertices, vertex_count, indices, index_count, false)) return;
    }
    if (drawNativeDynamicTexture(
            texture,
            vertices,
            vertex_count,
            indices,
            index_count,
            SCE_GXM_INDEX_FORMAT_U32,
            smoothing,
            wrap_mode)) {
        return;
    }
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
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
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
    if (!onRenderThread() && g_capture_index >= 0) {
        RenderCommand command{};
        command.type = RenderCommandType::Texture16;
        command.texture = texture;
        command.smoothing = smoothing;
        command.wrap_mode = wrap_mode;
        if (captureTextureDraw(command, vertices, vertex_count, indices, index_count, true)) return;
    }
    if (drawNativeDynamicTexture(
            texture,
            vertices,
            vertex_count,
            indices,
            index_count,
            SCE_GXM_INDEX_FORMAT_U16,
            smoothing,
            wrap_mode)) {
        return;
    }
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
    g_fallback_draws.fetch_add(1, std::memory_order_relaxed);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}

void flashvita_vitagl_set_async_pipeline(int32_t enabled) {
    const bool active = enabled != 0;
    if (!active) waitRenderIdleInternal();
    if (active && !g_render_thread_started.load(std::memory_order_acquire)) {
        // vitaShaRK/SceShaccCg is thread-affine in practice on Vita. Compile and
        // patch the native programs on the main thread before CPU1 is spawned;
        // the render worker only binds and submits already-created programs.
        nativeGxmLog("backend", "prewarm_main_begin");
        const bool native_ready = ensureNativeGxmPrograms();
        nativeGxmLog("backend", native_ready ? "prewarm_main_ready" : "prewarm_main_failed");
    }
    g_async_pipeline_enabled.store(active, std::memory_order_release);
    flashvita_vita_set_render_core_reserved(active ? 1 : 0);
}

int32_t flashvita_vitagl_commit_flash_frame(uint8_t present) {
    return commitCaptureFrame(present != 0);
}

void flashvita_vitagl_async_stats(uint64_t* submitted,
                                  uint64_t* executed,
                                  uint64_t* waits,
                                  uint64_t* native_draws,
                                  uint64_t* fallback_draws,
                                  uint64_t* native_texture_updates,
                                  uint64_t* gl_texture_updates,
                                  uint64_t* native_mask_ops,
                                  uint64_t* gl_mask_ops,
                                  uint64_t* native_offscreen_passes,
                                  uint64_t* gl_offscreen_passes) {
    if (submitted) *submitted = g_async_frames_submitted.load(std::memory_order_relaxed);
    if (executed) *executed = g_async_frames_executed.load(std::memory_order_relaxed);
    if (waits) *waits = g_async_waits.load(std::memory_order_relaxed);
    if (native_draws) *native_draws = g_native_draws.load(std::memory_order_relaxed);
    if (fallback_draws) *fallback_draws = g_fallback_draws.load(std::memory_order_relaxed);
    if (native_texture_updates) {
        *native_texture_updates = g_native_texture_updates.load(std::memory_order_relaxed);
    }
    if (gl_texture_updates) {
        *gl_texture_updates = g_gl_texture_updates.load(std::memory_order_relaxed);
    }
    if (native_mask_ops) *native_mask_ops = g_native_mask_ops.load(std::memory_order_relaxed);
    if (gl_mask_ops) *gl_mask_ops = g_gl_mask_ops.load(std::memory_order_relaxed);
    if (native_offscreen_passes) {
        *native_offscreen_passes = g_native_offscreen_passes.load(std::memory_order_relaxed);
    }
    if (gl_offscreen_passes) {
        *gl_offscreen_passes = g_gl_offscreen_passes.load(std::memory_order_relaxed);
    }
}

void flashvita_vitagl_wait_render_idle() {
    waitRenderIdleInternal();
}

void flashvita_vitagl_async_shutdown() {
    shutdownRenderThreadInternal();
}
} // extern "C"

namespace {

bool onRenderThread() {
    return g_render_thread_started.load(std::memory_order_acquire) &&
        sceKernelGetThreadId() == g_render_thread;
}

bool captureCommand(const RenderCommand& command) {
    if (g_capture_index < 0) return false;
    g_render_packets[g_capture_index].commands.push_back(command);
    return true;
}

bool captureColorDraw(RenderCommand command,
                      const FlashVitaRuffleVertex* vertices,
                      size_t vertex_count,
                      const void* indices,
                      size_t index_count,
                      bool indices_u16) {
    if (g_capture_index < 0 || !vertices || vertex_count == 0) return false;
    auto& packet = g_render_packets[g_capture_index];
    command.vertex_offset = static_cast<uint32_t>(packet.color_vertices.size());
    command.vertex_count = static_cast<uint32_t>(vertex_count);
    packet.color_vertices.insert(packet.color_vertices.end(), vertices, vertices + vertex_count);
    if (index_count != 0 && indices) {
        if (indices_u16) {
            command.index_offset = static_cast<uint32_t>(packet.indices16.size());
            const auto* src = static_cast<const uint16_t*>(indices);
            packet.indices16.insert(packet.indices16.end(), src, src + index_count);
        } else {
            command.index_offset = static_cast<uint32_t>(packet.indices32.size());
            const auto* src = static_cast<const uint32_t*>(indices);
            packet.indices32.insert(packet.indices32.end(), src, src + index_count);
        }
        command.index_count = index_count;
    }
    packet.commands.push_back(command);
    return true;
}

bool captureLineDraw(RenderCommand command,
                     const FlashVitaRuffleVertex* vertices,
                     size_t vertex_count) {
    if (g_capture_index < 0 || !vertices || vertex_count < 2) return false;
    auto& packet = g_render_packets[g_capture_index];
    command.vertex_offset = static_cast<uint32_t>(packet.color_vertices.size());
    command.vertex_count = static_cast<uint32_t>(vertex_count);
    packet.color_vertices.insert(packet.color_vertices.end(), vertices, vertices + vertex_count);

    const size_t index_count = (vertex_count - 1) * 2;
    command.index_count = index_count;
    if (vertex_count <= static_cast<size_t>(UINT16_MAX)) {
        command.arg0 = 16;
        command.index_offset = static_cast<uint32_t>(packet.indices16.size());
        packet.indices16.reserve(packet.indices16.size() + index_count);
        for (size_t i = 0; i + 1 < vertex_count; ++i) {
            packet.indices16.push_back(static_cast<uint16_t>(i));
            packet.indices16.push_back(static_cast<uint16_t>(i + 1));
        }
    } else {
        command.arg0 = 32;
        command.index_offset = static_cast<uint32_t>(packet.indices32.size());
        packet.indices32.reserve(packet.indices32.size() + index_count);
        for (size_t i = 0; i + 1 < vertex_count; ++i) {
            packet.indices32.push_back(static_cast<uint32_t>(i));
            packet.indices32.push_back(static_cast<uint32_t>(i + 1));
        }
    }
    packet.commands.push_back(command);
    return true;
}

bool captureTextureDraw(RenderCommand command,
                        const FlashVitaRuffleTexVertex* vertices,
                        size_t vertex_count,
                        const void* indices,
                        size_t index_count,
                        bool indices_u16) {
    if (g_capture_index < 0 || !vertices || vertex_count == 0) return false;
    auto& packet = g_render_packets[g_capture_index];
    command.vertex_offset = static_cast<uint32_t>(packet.tex_vertices.size());
    command.vertex_count = static_cast<uint32_t>(vertex_count);
    packet.tex_vertices.insert(packet.tex_vertices.end(), vertices, vertices + vertex_count);
    if (index_count != 0 && indices) {
        if (indices_u16) {
            command.index_offset = static_cast<uint32_t>(packet.indices16.size());
            const auto* src = static_cast<const uint16_t*>(indices);
            packet.indices16.insert(packet.indices16.end(), src, src + index_count);
        } else {
            command.index_offset = static_cast<uint32_t>(packet.indices32.size());
            const auto* src = static_cast<const uint32_t*>(indices);
            packet.indices32.insert(packet.indices32.end(), src, src + index_count);
        }
        command.index_count = index_count;
    }
    packet.commands.push_back(command);
    return true;
}

bool ensureVglStaging(void*& ptr, size_t& capacity, size_t bytes) {
    if (bytes == 0) return true;
    if (ptr && capacity >= bytes) return true;
    if (bytes > static_cast<size_t>(UINT32_MAX)) return false;
    void* replacement = vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_RAM);
    if (!replacement) return false;
    if (ptr) vglFree(ptr);
    ptr = replacement;
    capacity = bytes;
    return true;
}

bool drawNativeDynamicColor(const FlashVitaRuffleVertex* vertices,
                            size_t vertex_count,
                            const void* indices,
                            size_t index_count,
                            SceGxmIndexFormat index_format,
                            SceGxmPrimitiveType primitive) {
    if (!vertices || !indices || vertex_count == 0 || index_count == 0 ||
        !ensureNativeGxmPrograms()) {
        return false;
    }
    const size_t vertex_bytes = vertex_count * sizeof(FlashVitaRuffleVertex);
    const size_t index_bytes = index_count *
        (index_format == SCE_GXM_INDEX_FORMAT_U32 ? sizeof(uint32_t) : sizeof(uint16_t));
    if (!ensureVglStaging(
            g_sync_scratch.color_vertices, g_sync_scratch.color_capacity, vertex_bytes)) {
        return false;
    }
    void* index_ptr = index_format == SCE_GXM_INDEX_FORMAT_U32
        ? static_cast<void*>(g_sync_scratch.indices32)
        : static_cast<void*>(g_sync_scratch.indices16);
    size_t* index_capacity = index_format == SCE_GXM_INDEX_FORMAT_U32
        ? &g_sync_scratch.indices32_capacity
        : &g_sync_scratch.indices16_capacity;
    if (!ensureVglStaging(index_ptr, *index_capacity, index_bytes)) return false;
    if (index_format == SCE_GXM_INDEX_FORMAT_U32) {
        g_sync_scratch.indices32 = static_cast<uint32_t*>(index_ptr);
    } else {
        g_sync_scratch.indices16 = static_cast<uint16_t*>(index_ptr);
    }

    if (sceDmacMemcpy(
            g_sync_scratch.color_vertices, vertices, static_cast<SceSize>(vertex_bytes)) < 0) {
        std::memcpy(g_sync_scratch.color_vertices, vertices, vertex_bytes);
    }
    if (sceDmacMemcpy(index_ptr, indices, static_cast<SceSize>(index_bytes)) < 0) {
        std::memcpy(index_ptr, indices, index_bytes);
    }

    NativeGpuMesh mesh{};
    mesh.vertices = g_sync_scratch.color_vertices;
    mesh.indices = index_ptr;
    mesh.vertex_bytes = vertex_bytes;
    mesh.index_count = index_count;
    mesh.index_format = index_format;
    mesh.primitive = primitive;
    mesh.native = true;
    return drawNativeGxmMesh(&mesh, index_count, 0, identityGpuTransform(), 0, 0);
}

bool drawNativeDynamicTexture(uint32_t texture,
                              const FlashVitaRuffleTexVertex* vertices,
                              size_t vertex_count,
                              const void* indices,
                              size_t index_count,
                              SceGxmIndexFormat index_format,
                              uint8_t smoothing,
                              uint8_t wrap_mode) {
    if (!texture || !vertices || !indices || vertex_count == 0 || index_count == 0 ||
        !ensureNativeGxmPrograms()) {
        return false;
    }
    const size_t vertex_bytes = vertex_count * sizeof(FlashVitaRuffleTexVertex);
    const size_t index_bytes = index_count *
        (index_format == SCE_GXM_INDEX_FORMAT_U32 ? sizeof(uint32_t) : sizeof(uint16_t));
    if (!ensureVglStaging(
            g_sync_scratch.tex_vertices, g_sync_scratch.tex_capacity, vertex_bytes)) {
        return false;
    }
    void* index_ptr = index_format == SCE_GXM_INDEX_FORMAT_U32
        ? static_cast<void*>(g_sync_scratch.indices32)
        : static_cast<void*>(g_sync_scratch.indices16);
    size_t* index_capacity = index_format == SCE_GXM_INDEX_FORMAT_U32
        ? &g_sync_scratch.indices32_capacity
        : &g_sync_scratch.indices16_capacity;
    if (!ensureVglStaging(index_ptr, *index_capacity, index_bytes)) return false;
    if (index_format == SCE_GXM_INDEX_FORMAT_U32) {
        g_sync_scratch.indices32 = static_cast<uint32_t*>(index_ptr);
    } else {
        g_sync_scratch.indices16 = static_cast<uint16_t*>(index_ptr);
    }

    if (sceDmacMemcpy(
            g_sync_scratch.tex_vertices, vertices, static_cast<SceSize>(vertex_bytes)) < 0) {
        std::memcpy(g_sync_scratch.tex_vertices, vertices, vertex_bytes);
    }
    if (sceDmacMemcpy(index_ptr, indices, static_cast<SceSize>(index_bytes)) < 0) {
        std::memcpy(index_ptr, indices, index_bytes);
    }

    NativeGpuMesh mesh{};
    mesh.vertices = g_sync_scratch.tex_vertices;
    mesh.indices = index_ptr;
    mesh.vertex_bytes = vertex_bytes;
    mesh.index_count = index_count;
    mesh.index_format = index_format;
    mesh.primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    mesh.native = true;
    return drawNativeGxmMesh(
        &mesh, index_count, texture, identityGpuTransform(), smoothing, wrap_mode);
}

bool drawNativeDynamicLine(const FlashVitaRuffleVertex* vertices, size_t vertex_count) {
    if (!vertices || vertex_count < 2) return false;
    const size_t index_count = (vertex_count - 1) * 2;
    if (vertex_count <= static_cast<size_t>(UINT16_MAX)) {
        auto& indices = g_sync_scratch.line_indices16;
        indices.resize(index_count);
        for (size_t i = 0, out = 0; i + 1 < vertex_count; ++i) {
            indices[out++] = static_cast<uint16_t>(i);
            indices[out++] = static_cast<uint16_t>(i + 1);
        }
        return drawNativeDynamicColor(
            vertices,
            vertex_count,
            indices.data(),
            index_count,
            SCE_GXM_INDEX_FORMAT_U16,
            SCE_GXM_PRIMITIVE_LINES);
    }

    auto& indices = g_sync_scratch.line_indices32;
    indices.resize(index_count);
    for (size_t i = 0, out = 0; i + 1 < vertex_count; ++i) {
        indices[out++] = static_cast<uint32_t>(i);
        indices[out++] = static_cast<uint32_t>(i + 1);
    }
    return drawNativeDynamicColor(
        vertices,
        vertex_count,
        indices.data(),
        index_count,
        SCE_GXM_INDEX_FORMAT_U32,
        SCE_GXM_PRIMITIVE_LINES);
}

bool ensurePacketGpuStaging(RenderPacket& packet) {
    const size_t color_bytes = packet.color_vertices.size() * sizeof(FlashVitaRuffleVertex);
    const size_t tex_bytes = packet.tex_vertices.size() * sizeof(FlashVitaRuffleTexVertex);
    const size_t index16_bytes = packet.indices16.size() * sizeof(uint16_t);
    const size_t index32_bytes = packet.indices32.size() * sizeof(uint32_t);
    if (!ensureVglStaging(packet.gpu_color_vertices, packet.gpu_color_capacity, color_bytes) ||
        !ensureVglStaging(packet.gpu_tex_vertices, packet.gpu_tex_capacity, tex_bytes)) {
        return false;
    }
    void* index16_ptr = packet.gpu_indices16;
    void* index32_ptr = packet.gpu_indices32;
    if (!ensureVglStaging(index16_ptr, packet.gpu_indices16_capacity, index16_bytes) ||
        !ensureVglStaging(index32_ptr, packet.gpu_indices32_capacity, index32_bytes)) {
        return false;
    }
    packet.gpu_indices16 = static_cast<uint16_t*>(index16_ptr);
    packet.gpu_indices32 = static_cast<uint32_t*>(index32_ptr);

    if (color_bytes != 0) {
        if (sceDmacMemcpy(packet.gpu_color_vertices, packet.color_vertices.data(),
                          static_cast<SceSize>(color_bytes)) < 0) {
            std::memcpy(packet.gpu_color_vertices, packet.color_vertices.data(), color_bytes);
        }
    }
    if (tex_bytes != 0) {
        if (sceDmacMemcpy(packet.gpu_tex_vertices, packet.tex_vertices.data(),
                          static_cast<SceSize>(tex_bytes)) < 0) {
            std::memcpy(packet.gpu_tex_vertices, packet.tex_vertices.data(), tex_bytes);
        }
    }
    if (index16_bytes != 0) {
        if (sceDmacMemcpy(packet.gpu_indices16, packet.indices16.data(),
                          static_cast<SceSize>(index16_bytes)) < 0) {
            std::memcpy(packet.gpu_indices16, packet.indices16.data(), index16_bytes);
        }
    }
    if (index32_bytes != 0) {
        if (sceDmacMemcpy(packet.gpu_indices32, packet.indices32.data(),
                          static_cast<SceSize>(index32_bytes)) < 0) {
            std::memcpy(packet.gpu_indices32, packet.indices32.data(), index32_bytes);
        }
    }
    return true;
}

FlashVitaGpuTransform identityGpuTransform() {
    FlashVitaGpuTransform transform{};
    transform.a = 1.0f;
    transform.d = 1.0f;
    transform.mult[0] = 1.0f;
    transform.mult[1] = 1.0f;
    transform.mult[2] = 1.0f;
    transform.mult[3] = 1.0f;
    return transform;
}

void executeRenderPacket(RenderPacket& packet) {
    const bool native_staging_ready = ensurePacketGpuStaging(packet);
    const FlashVitaGpuTransform identity = identityGpuTransform();
    for (const auto& command : packet.commands) {
        switch (command.type) {
            case RenderCommandType::BeginFrame:
                flashvita_vitagl_begin_flash_frame(
                    command.clear[0], command.clear[1], command.clear[2], command.clear[3]);
                break;
            case RenderCommandType::GpuColor:
                flashvita_vitagl_draw_gpu_colored(
                    command.handle, command.index_count, &command.transform);
                break;
            case RenderCommandType::GpuTexture:
                flashvita_vitagl_draw_gpu_textured(
                    command.handle,
                    command.index_count,
                    command.texture,
                    &command.transform,
                    command.smoothing,
                    command.wrap_mode);
                break;
            case RenderCommandType::MaskPush:
                flashvita_vitagl_mask_push(command.arg0);
                break;
            case RenderCommandType::MaskActivate:
                flashvita_vitagl_mask_activate(command.arg0);
                break;
            case RenderCommandType::MaskDeactivate:
                flashvita_vitagl_mask_deactivate(command.arg0);
                break;
            case RenderCommandType::MaskPop:
                flashvita_vitagl_mask_pop(command.arg0);
                break;
            case RenderCommandType::Color32: {
                bool native = false;
                if (native_staging_ready && packet.gpu_color_vertices && packet.gpu_indices32) {
                    auto* vertices = static_cast<FlashVitaRuffleVertex*>(packet.gpu_color_vertices) +
                        command.vertex_offset;
                    auto* indices = packet.gpu_indices32 + command.index_offset;
                    NativeGpuMesh mesh{};
                    mesh.vertices = vertices;
                    mesh.indices = indices;
                    mesh.vertex_bytes = command.vertex_count * sizeof(FlashVitaRuffleVertex);
                    mesh.index_count = command.index_count;
                    mesh.index_format = SCE_GXM_INDEX_FORMAT_U32;
                    mesh.primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
                    mesh.native = true;
                    native = drawNativeGxmMesh(
                        &mesh, command.index_count, 0, identity, 0, 0);
                }
                if (!native) {
                    const auto* vertices = packet.color_vertices.data() + command.vertex_offset;
                    const auto* indices = packet.indices32.data() + command.index_offset;
                    flashvita_vitagl_draw_colored_triangles(
                        vertices, command.vertex_count, indices, command.index_count);
                }
                break;
            }
            case RenderCommandType::Color16: {
                bool native = false;
                if (native_staging_ready && packet.gpu_color_vertices && packet.gpu_indices16) {
                    auto* vertices = static_cast<FlashVitaRuffleVertex*>(packet.gpu_color_vertices) +
                        command.vertex_offset;
                    auto* indices = packet.gpu_indices16 + command.index_offset;
                    NativeGpuMesh mesh{};
                    mesh.vertices = vertices;
                    mesh.indices = indices;
                    mesh.vertex_bytes = command.vertex_count * sizeof(FlashVitaRuffleVertex);
                    mesh.index_count = command.index_count;
                    mesh.native = true;
                    native = drawNativeGxmMesh(
                        &mesh, command.index_count, 0, identity, 0, 0);
                }
                if (!native) {
                    const auto* vertices = packet.color_vertices.data() + command.vertex_offset;
                    const auto* indices = packet.indices16.data() + command.index_offset;
                    flashvita_vitagl_draw_colored_triangles_u16(
                        vertices, command.vertex_count, indices, command.index_count);
                }
                break;
            }
            case RenderCommandType::ColorLine: {
                bool native = false;
                if (native_staging_ready && packet.gpu_color_vertices) {
                    auto* vertices = static_cast<FlashVitaRuffleVertex*>(packet.gpu_color_vertices) +
                        command.vertex_offset;
                    NativeGpuMesh mesh{};
                    mesh.vertices = vertices;
                    mesh.vertex_bytes = command.vertex_count * sizeof(FlashVitaRuffleVertex);
                    mesh.index_count = command.index_count;
                    mesh.primitive = SCE_GXM_PRIMITIVE_LINES;
                    mesh.native = true;
                    if (command.arg0 == 32 && packet.gpu_indices32) {
                        mesh.indices = packet.gpu_indices32 + command.index_offset;
                        mesh.index_format = SCE_GXM_INDEX_FORMAT_U32;
                        native = drawNativeGxmMesh(
                            &mesh, command.index_count, 0, identity, 0, 0);
                    } else if (command.arg0 == 16 && packet.gpu_indices16) {
                        mesh.indices = packet.gpu_indices16 + command.index_offset;
                        mesh.index_format = SCE_GXM_INDEX_FORMAT_U16;
                        native = drawNativeGxmMesh(
                            &mesh, command.index_count, 0, identity, 0, 0);
                    }
                }
                if (!native) {
                    const auto* vertices = packet.color_vertices.data() + command.vertex_offset;
                    flashvita_vitagl_draw_colored_line_strip(vertices, command.vertex_count);
                }
                break;
            }
            case RenderCommandType::Texture32: {
                bool native = false;
                if (native_staging_ready && packet.gpu_tex_vertices && packet.gpu_indices32) {
                    auto* vertices = static_cast<FlashVitaRuffleTexVertex*>(packet.gpu_tex_vertices) +
                        command.vertex_offset;
                    auto* indices = packet.gpu_indices32 + command.index_offset;
                    NativeGpuMesh mesh{};
                    mesh.vertices = vertices;
                    mesh.indices = indices;
                    mesh.vertex_bytes = command.vertex_count * sizeof(FlashVitaRuffleTexVertex);
                    mesh.index_count = command.index_count;
                    mesh.index_format = SCE_GXM_INDEX_FORMAT_U32;
                    mesh.primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
                    mesh.native = true;
                    native = drawNativeGxmMesh(
                        &mesh,
                        command.index_count,
                        command.texture,
                        identity,
                        command.smoothing,
                        command.wrap_mode);
                }
                if (!native) {
                    const auto* vertices = packet.tex_vertices.data() + command.vertex_offset;
                    const auto* indices = packet.indices32.data() + command.index_offset;
                    flashvita_vitagl_draw_textured_triangles(
                        command.texture,
                        vertices,
                        command.vertex_count,
                        indices,
                        command.index_count,
                        command.smoothing,
                        command.wrap_mode);
                }
                break;
            }
            case RenderCommandType::Texture16: {
                bool native = false;
                if (native_staging_ready && packet.gpu_tex_vertices && packet.gpu_indices16) {
                    auto* vertices = static_cast<FlashVitaRuffleTexVertex*>(packet.gpu_tex_vertices) +
                        command.vertex_offset;
                    auto* indices = packet.gpu_indices16 + command.index_offset;
                    NativeGpuMesh mesh{};
                    mesh.vertices = vertices;
                    mesh.indices = indices;
                    mesh.vertex_bytes = command.vertex_count * sizeof(FlashVitaRuffleTexVertex);
                    mesh.index_count = command.index_count;
                    mesh.native = true;
                    native = drawNativeGxmMesh(
                        &mesh,
                        command.index_count,
                        command.texture,
                        identity,
                        command.smoothing,
                        command.wrap_mode);
                }
                if (!native) {
                    const auto* vertices = packet.tex_vertices.data() + command.vertex_offset;
                    const auto* indices = packet.indices16.data() + command.index_offset;
                    flashvita_vitagl_draw_textured_triangles_u16(
                        command.texture,
                        vertices,
                        command.vertex_count,
                        indices,
                        command.index_count,
                        command.smoothing,
                        command.wrap_mode);
                }
                break;
            }
        }
    }
    if (packet.present) vglSwapBuffers(GL_FALSE);
}

int renderThreadEntry(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(g_render_ready_sema, 1, nullptr);
        if (g_render_thread_stop.load(std::memory_order_acquire) &&
            g_render_pending.load(std::memory_order_acquire) == 0) {
            return 0;
        }

        const int index = g_render_read_index;
        if (g_render_packet_state[index].load(std::memory_order_acquire) != 2) {
            continue;
        }
        executeRenderPacket(g_render_packets[index]);
        g_async_frames_executed.fetch_add(1, std::memory_order_relaxed);
        g_render_packets[index].reset();
        g_render_packet_state[index].store(0, std::memory_order_release);
        g_render_read_index = (g_render_read_index + 1) & 1;
        g_render_pending.fetch_sub(1, std::memory_order_acq_rel);
        sceKernelSignalSema(g_render_done_sema, 1);
    }
}

bool ensureRenderThread() {
    if (!g_async_pipeline_enabled.load(std::memory_order_acquire)) return false;
    if (g_render_thread_started.load(std::memory_order_acquire)) return true;

    g_render_ready_sema = sceKernelCreateSema("FlashVitaRenderReady", 0, 0, 2, nullptr);
    g_render_done_sema = sceKernelCreateSema("FlashVitaRenderDone", 0, 0, 2, nullptr);
    if (g_render_ready_sema < 0 || g_render_done_sema < 0) {
        if (g_render_ready_sema >= 0) sceKernelDeleteSema(g_render_ready_sema);
        if (g_render_done_sema >= 0) sceKernelDeleteSema(g_render_done_sema);
        g_render_ready_sema = g_render_done_sema = -1;
        return false;
    }

    const int base_priority = sceKernelGetThreadCurrentPriority();
    const int render_priority = base_priority >= 0 ? base_priority : 0x10000100;
    g_render_thread = sceKernelCreateThread(
        "FlashVitaRender",
        renderThreadEntry,
        render_priority,
        256 * 1024,
        0,
        SCE_KERNEL_CPU_MASK_USER_1,
        nullptr);
    if (g_render_thread < 0 || sceKernelStartThread(g_render_thread, 0, nullptr) < 0) {
        if (g_render_thread >= 0) sceKernelDeleteThread(g_render_thread);
        sceKernelDeleteSema(g_render_ready_sema);
        sceKernelDeleteSema(g_render_done_sema);
        g_render_thread = g_render_ready_sema = g_render_done_sema = -1;
        return false;
    }
    g_render_thread_stop.store(false, std::memory_order_release);
    g_render_thread_started.store(true, std::memory_order_release);
    return true;
}

void waitRenderIdleInternal() {
    if (!g_render_thread_started.load(std::memory_order_acquire) || onRenderThread()) return;
    if (g_render_pending.load(std::memory_order_acquire) > 0) {
        g_async_waits.fetch_add(1, std::memory_order_relaxed);
    }
    while (g_render_pending.load(std::memory_order_acquire) > 0) {
        sceKernelWaitSema(g_render_done_sema, 1, nullptr);
    }
}

bool beginCaptureFrame(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ensureRenderThread()) return false;
    if (g_capture_index >= 0) return true;

    int index = g_render_write_index;
    if (g_render_packet_state[index].load(std::memory_order_acquire) != 0) {
        waitRenderIdleInternal();
    }
    if (g_render_packet_state[index].load(std::memory_order_acquire) != 0) return false;

    auto& packet = g_render_packets[index];
    packet.reset();
    g_render_packet_state[index].store(1, std::memory_order_release);
    g_capture_index = index;
    RenderCommand command{};
    command.type = RenderCommandType::BeginFrame;
    command.clear[0] = r;
    command.clear[1] = g;
    command.clear[2] = b;
    command.clear[3] = a;
    packet.commands.push_back(command);
    return true;
}

int32_t commitCaptureFrame(bool present) {
    if (g_capture_index < 0) return 0;
    const int index = g_capture_index;
    auto& packet = g_render_packets[index];
    packet.present = present;
    g_capture_index = -1;
    g_render_packet_state[index].store(2, std::memory_order_release);
    g_render_write_index = (g_render_write_index + 1) & 1;
    g_render_pending.fetch_add(1, std::memory_order_acq_rel);
    g_async_frames_submitted.fetch_add(1, std::memory_order_relaxed);
    sceKernelSignalSema(g_render_ready_sema, 1);
    return 1;
}

void shutdownRenderThreadInternal() {
    if (!g_render_thread_started.load(std::memory_order_acquire)) return;
    if (g_capture_index >= 0) commitCaptureFrame(false);
    waitRenderIdleInternal();
    g_render_thread_started.store(false, std::memory_order_release);
    g_render_thread_stop.store(true, std::memory_order_release);
    sceKernelSignalSema(g_render_ready_sema, 1);
    sceKernelWaitThreadEnd(g_render_thread, nullptr, nullptr);
    sceKernelDeleteThread(g_render_thread);
    sceKernelDeleteSema(g_render_ready_sema);
    sceKernelDeleteSema(g_render_done_sema);
    g_render_thread = g_render_ready_sema = g_render_done_sema = -1;
    g_render_write_index = g_render_read_index = 0;
    g_capture_index = -1;
    for (auto& packet : g_render_packets) {
        if (packet.gpu_color_vertices) vglFree(packet.gpu_color_vertices);
        if (packet.gpu_tex_vertices) vglFree(packet.gpu_tex_vertices);
        if (packet.gpu_indices16) vglFree(packet.gpu_indices16);
        if (packet.gpu_indices32) vglFree(packet.gpu_indices32);
        packet.gpu_color_vertices = nullptr;
        packet.gpu_tex_vertices = nullptr;
        packet.gpu_indices16 = nullptr;
        packet.gpu_indices32 = nullptr;
        packet.gpu_color_capacity = 0;
        packet.gpu_tex_capacity = 0;
        packet.gpu_indices16_capacity = 0;
        packet.gpu_indices32_capacity = 0;
        packet.reset();
    }
    flashvita_vita_set_render_core_reserved(0);
}

} // namespace
#endif
