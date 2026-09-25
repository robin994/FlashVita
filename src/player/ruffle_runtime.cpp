#include "ruffle_runtime.h"

#include <cstdio>
#include <vector>

#if FLASHVITA_ENABLE_RUFFLE
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#endif

#if FLASHVITA_ENABLE_RUFFLE
namespace {
bool g_flash_texture_enabled = false;
bool g_flash_texcoord_array = false;
bool g_flash_color_array = false;
GLuint g_flash_bound_texture = 0;
int g_flash_filter = -1;
int g_flash_wrap = -1;

void invalidateFlashGlCache() {
    g_flash_texture_enabled = false;
    g_flash_texcoord_array = false;
    g_flash_color_array = false;
    g_flash_bound_texture = 0;
    g_flash_filter = -1;
    g_flash_wrap = -1;
}
}

extern "C" {

struct FlashVitaRuffleProbe {
    uint8_t version;
    uint8_t compression;
    uint8_t has_avm1;
    uint8_t has_avm2;
    uint16_t frame_count;
    uint16_t reserved;
    uint32_t tag_count;
    int32_t stage_width;
    int32_t stage_height;
    float frame_rate;
};

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

struct FlashVitaRendererStats {
    uint64_t frames;
    uint64_t colored_draws;
    uint64_t textured_draws;
    uint64_t bitmap_uploads;
    uint64_t lines;
    uint64_t gradient_skips;
    uint64_t missing_bitmaps;
    uint64_t mask_ops;
    uint64_t blends;
    uint64_t stage3d;
};

const char* flashvita_ruffle_bridge_version();
int32_t flashvita_ruffle_probe(const uint8_t* data, size_t len, FlashVitaRuffleProbe* out);
void* flashvita_ruffle_headless_create(const uint8_t* data, size_t len, FlashVitaRuffleProbe* out);
int32_t flashvita_ruffle_headless_tick(void* handle, double dt_ms);
int32_t flashvita_ruffle_key_event(void* handle, int32_t key, uint8_t down);
int32_t flashvita_ruffle_mouse_move(void* handle, double x, double y);
int32_t flashvita_ruffle_mouse_button(void* handle, double x, double y, uint8_t down);
void flashvita_ruffle_headless_destroy(void* handle);
int32_t flashvita_ruffle_renderer_stats(FlashVitaRendererStats* out);

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
    // Ruffle currently hands us the complete bitmap even when only a dirty region changed.
    // A full re-upload is correct and keeps the first Vita backend simple; optimize later.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
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
}
#endif

namespace flashvita {
namespace {

#if FLASHVITA_ENABLE_RUFFLE
bool readWholeFile(const std::string& path, std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        std::fclose(file);
        return false;
    }

    bytes.resize(static_cast<size_t>(length));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        bytes.clear();
        return false;
    }
    return true;
}
#endif

#if FLASHVITA_ENABLE_RUFFLE
void copyProbe(const FlashVitaRuffleProbe& src, RuffleProbeInfo& dst) {
    dst.available = true;
    dst.parsed = true;
    dst.version = src.version;
    dst.compression = src.compression;
    dst.has_avm1 = src.has_avm1 != 0;
    dst.has_avm2 = src.has_avm2 != 0;
    dst.frame_count = src.frame_count;
    dst.tag_count = src.tag_count;
    dst.stage_width = src.stage_width;
    dst.stage_height = src.stage_height;
    dst.frame_rate = src.frame_rate;
}

void logRendererStats(const char* reason) {
    FlashVitaRendererStats stats{};
    if (flashvita_ruffle_renderer_stats(&stats) != 0) return;
    FILE* f = std::fopen("ux0:data/FlashVita/runtime.log", "a");
    if (!f) return;
    std::fprintf(f,
                 "ruffle_renderer reason=%s frames=%llu color=%llu textured=%llu uploads=%llu "
                 "lines=%llu gradient_skip=%llu missing_bitmap=%llu mask_ops=%llu blends=%llu stage3d=%llu\n",
                 reason ? reason : "periodic",
                 static_cast<unsigned long long>(stats.frames),
                 static_cast<unsigned long long>(stats.colored_draws),
                 static_cast<unsigned long long>(stats.textured_draws),
                 static_cast<unsigned long long>(stats.bitmap_uploads),
                 static_cast<unsigned long long>(stats.lines),
                 static_cast<unsigned long long>(stats.gradient_skips),
                 static_cast<unsigned long long>(stats.missing_bitmaps),
                 static_cast<unsigned long long>(stats.mask_ops),
                 static_cast<unsigned long long>(stats.blends),
                 static_cast<unsigned long long>(stats.stage3d));
    std::fclose(f);
}

void logRuntimePerf(uint32_t ticks, uint32_t rendered, uint64_t total_us, uint64_t max_us) {
    if (ticks == 0) return;
    FILE* f = std::fopen("ux0:data/FlashVita/runtime.log", "a");
    if (!f) return;
    std::fprintf(f,
                 "ruffle_perf ticks=%u rendered=%u avg_tick_render_us=%llu max_tick_render_us=%llu budget60_us=16667\n",
                 ticks,
                 rendered,
                 static_cast<unsigned long long>(total_us / ticks),
                 static_cast<unsigned long long>(max_us));
    std::fclose(f);
}
#endif

} // namespace

RuffleRuntime::~RuffleRuntime() {
    stop();
}

bool RuffleRuntime::compiledIn() {
#if FLASHVITA_ENABLE_RUFFLE
    return true;
#else
    return false;
#endif
}

const char* RuffleRuntime::bridgeVersion() {
#if FLASHVITA_ENABLE_RUFFLE
    const char* version = flashvita_ruffle_bridge_version();
    return version ? version : "Ruffle bridge (unknown version)";
#else
    return "Ruffle bridge not linked";
#endif
}

void RuffleRuntime::prepareUiGraphics() {
#if FLASHVITA_ENABLE_RUFFLE
    flashvita_vitagl_prepare_ui();
#endif
}

bool RuffleRuntime::probeFile(const std::string& path, RuffleProbeInfo& out) {
    out = RuffleProbeInfo{};
#if FLASHVITA_ENABLE_RUFFLE
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) {
        out.available = true;
        out.message = "Ruffle: unable to read SWF file";
        return false;
    }

    FlashVitaRuffleProbe probe{};
    const int32_t result = flashvita_ruffle_probe(bytes.data(), bytes.size(), &probe);
    out.available = true;
    if (result != 0) {
        out.message = "Ruffle: SWF parser rejected the file";
        return false;
    }
    copyProbe(probe, out);
    out.message = "Ruffle SWF parser: OK";
    return true;
#else
    (void)path;
    out.message = "Ruffle bridge is prepared but not linked: install a current nightly Rust + cargo-vita.";
    return false;
#endif
}

bool RuffleRuntime::startHeadless(const std::string& path, RuffleProbeInfo& out) {
    stop();
    out = RuffleProbeInfo{};
#if FLASHVITA_ENABLE_RUFFLE
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) {
        out.available = true;
        out.message = "Ruffle: unable to read SWF file";
        return false;
    }

    FlashVitaRuffleProbe probe{};
    handle_ = flashvita_ruffle_headless_create(bytes.data(), bytes.size(), &probe);
    out.available = true;
    if (!handle_) {
        out.message = "Ruffle: failed to construct headless Player";
        return false;
    }
    copyProbe(probe, out);
    out.message = "Ruffle Player constructed with FlashVita vitaGL renderer";
    tick_counter_ = 0;
    rendered_last_tick_ = false;
    perf_total_us_ = 0;
    perf_max_us_ = 0;
    perf_ticks_ = 0;
    perf_rendered_ = 0;
    logRendererStats("start");
    return true;
#else
    return probeFile(path, out);
#endif
}

bool RuffleRuntime::tick(double dt_ms) {
#if FLASHVITA_ENABLE_RUFFLE
    if (!handle_) return false;
    const uint64_t begin_us = sceKernelGetProcessTimeWide();
    const int32_t result = flashvita_ruffle_headless_tick(handle_, dt_ms);
    const uint64_t elapsed_us = sceKernelGetProcessTimeWide() - begin_us;
    if (result < 0) return false;
    rendered_last_tick_ = result > 0;
    ++tick_counter_;
    perf_total_us_ += elapsed_us;
    if (elapsed_us > perf_max_us_) perf_max_us_ = elapsed_us;
    ++perf_ticks_;
    if (rendered_last_tick_) ++perf_rendered_;
    if (perf_ticks_ >= 300) {
        logRuntimePerf(perf_ticks_, perf_rendered_, perf_total_us_, perf_max_us_);
        perf_total_us_ = 0;
        perf_max_us_ = 0;
        perf_ticks_ = 0;
        perf_rendered_ = 0;
    }
    if (tick_counter_ == 1 || (tick_counter_ % 600) == 0) {
        logRendererStats(tick_counter_ == 1 ? "first_frame" : "periodic");
    }
    return true;
#else
    (void)dt_ms;
    return false;
#endif
}

bool RuffleRuntime::keyEvent(int key, bool down) {
#if FLASHVITA_ENABLE_RUFFLE
    return handle_ && flashvita_ruffle_key_event(handle_, key, down ? 1 : 0) == 0;
#else
    (void)key;
    (void)down;
    return false;
#endif
}

bool RuffleRuntime::mouseMove(double x, double y) {
#if FLASHVITA_ENABLE_RUFFLE
    return handle_ && flashvita_ruffle_mouse_move(handle_, x, y) == 0;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

bool RuffleRuntime::mouseButton(double x, double y, bool down) {
#if FLASHVITA_ENABLE_RUFFLE
    return handle_ && flashvita_ruffle_mouse_button(handle_, x, y, down ? 1 : 0) == 0;
#else
    (void)x;
    (void)y;
    (void)down;
    return false;
#endif
}

void RuffleRuntime::stop() {
#if FLASHVITA_ENABLE_RUFFLE
    if (handle_) flashvita_ruffle_headless_destroy(handle_);
#endif
    handle_ = nullptr;
    tick_counter_ = 0;
    rendered_last_tick_ = false;
    perf_total_us_ = 0;
    perf_max_us_ = 0;
    perf_ticks_ = 0;
    perf_rendered_ = 0;
}

} // namespace flashvita
