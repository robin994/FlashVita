#include "ruffle_runtime.h"
#include "vitagl_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <malloc.h>
#include <new>
#include <vector>

#if FLASHVITA_ENABLE_RUFFLE
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>
#endif

#include "../platform/vita_native.h"

#if FLASHVITA_ENABLE_RUFFLE
namespace {
void logMemoryStats(const char* stage) {
    const struct mallinfo heap = mallinfo();
    char line[512];
    const int length = sceClibSnprintf(
        line,
        sizeof(line),
        "memory stage=%s arena=%u used=%u free=%u top=%u "
        "vgl_vram_free=%llu vgl_ram_free=%llu vgl_phycont_free=%llu "
        "vgl_budget_free=%llu vgl_external_free=%llu\n",
        stage ? stage : "unknown",
        static_cast<unsigned>(heap.arena),
        static_cast<unsigned>(heap.uordblks),
        static_cast<unsigned>(heap.fordblks),
        static_cast<unsigned>(heap.keepcost),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_VRAM)),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_RAM)),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_PHYCONT)),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_BUDGET)),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_EXTERNAL)));
    if (length > 0) {
        flashvita::vita::appendFile(
            "ux0:data/FlashVita/runtime.log",
            line,
            static_cast<size_t>(length));
    }
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

struct FlashVitaRendererStats {
    uint64_t frames;
    uint64_t colored_draws;
    uint64_t textured_draws;
    uint64_t bitmap_uploads;
    uint64_t bitmap_partial_uploads;
    uint64_t bitmap_uploaded_bytes;
    uint64_t lines;
    uint64_t gradient_skips;
    uint64_t missing_bitmaps;
    uint64_t mask_ops;
    uint64_t blends;
    uint64_t stage3d;
    uint64_t transformed_vertices;
    uint64_t parallel_draws;
    uint64_t parallel_batches;
    uint64_t parallel_jobs;
    uint64_t color_submissions;
    uint64_t prepass_us;
    uint64_t submit_us;
};

struct FlashVitaTextInputInfo {
    uint8_t multiline;
    uint8_t password;
    uint8_t reserved[2];
    uint32_t max_length;
    uint32_t initial_text_bytes;
};

const char* flashvita_ruffle_bridge_version();
int32_t flashvita_ruffle_probe(const uint8_t* data, size_t len, FlashVitaRuffleProbe* out);
void* flashvita_ruffle_headless_create(const char* swf_path,
                                      const char* web_movie_url,
                                      const char* air_movie_url,
                                      const char* cache_root,
                                      FlashVitaRuffleProbe* out);
int32_t flashvita_ruffle_headless_update(void* handle, double dt_ms);
int32_t flashvita_ruffle_headless_tick(void* handle, double dt_ms);
int32_t flashvita_ruffle_headless_render(void* handle);
int32_t flashvita_ruffle_key_event(void* handle, int32_t key, uint8_t down);
int32_t flashvita_ruffle_mouse_move(void* handle, double x, double y);
int32_t flashvita_ruffle_mouse_button(void* handle, double x, double y, uint8_t down);
int32_t flashvita_ruffle_mouse_leave(void* handle);
int32_t flashvita_ruffle_text_input_info(void* handle, FlashVitaTextInputInfo* out,
                                        uint8_t* initial_text, size_t initial_text_capacity);
int32_t flashvita_ruffle_virtual_keyboard_ack(void* handle);
int32_t flashvita_ruffle_replace_focused_text(void* handle, const uint8_t* data, size_t len);
void flashvita_ruffle_headless_destroy(void* handle);
int32_t flashvita_ruffle_renderer_stats(FlashVitaRendererStats* out);
}
#endif

namespace flashvita {
namespace {

#if FLASHVITA_ENABLE_RUFFLE
constexpr size_t kImeUtf16Capacity = SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1;
constexpr size_t kImeUtf8Capacity = SCE_IME_DIALOG_MAX_TEXT_LENGTH * 4 + 1;
const SceWChar16 kImeTitle[] = {
    'E', 'n', 't', 'e', 'r', ' ', 't', 'e', 'x', 't', 0,
};

void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

void utf8ToUtf16(const uint8_t* input, uint16_t* output, size_t output_capacity) {
    if (!output || output_capacity == 0) return;
    output[0] = 0;
    if (!input) return;

    size_t in = 0;
    size_t out = 0;
    while (input[in] != 0 && out + 1 < output_capacity) {
        uint32_t codepoint = 0xFFFD;
        const uint8_t first = input[in];
        size_t consumed = 1;
        if (first < 0x80) {
            codepoint = first;
        } else if ((first & 0xE0) == 0xC0 && (input[in + 1] & 0xC0) == 0x80) {
            codepoint = ((first & 0x1F) << 6) | (input[in + 1] & 0x3F);
            consumed = 2;
        } else if ((first & 0xF0) == 0xE0 &&
                   (input[in + 1] & 0xC0) == 0x80 &&
                   (input[in + 2] & 0xC0) == 0x80) {
            codepoint = ((first & 0x0F) << 12) |
                        ((input[in + 1] & 0x3F) << 6) |
                        (input[in + 2] & 0x3F);
            consumed = 3;
        } else if ((first & 0xF8) == 0xF0 &&
                   (input[in + 1] & 0xC0) == 0x80 &&
                   (input[in + 2] & 0xC0) == 0x80 &&
                   (input[in + 3] & 0xC0) == 0x80) {
            codepoint = ((first & 0x07) << 18) |
                        ((input[in + 1] & 0x3F) << 12) |
                        ((input[in + 2] & 0x3F) << 6) |
                        (input[in + 3] & 0x3F);
            consumed = 4;
        }
        in += consumed;

        if (codepoint <= 0xFFFF) {
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) codepoint = 0xFFFD;
            output[out++] = static_cast<uint16_t>(codepoint);
        } else if (codepoint <= 0x10FFFF && out + 2 < output_capacity) {
            codepoint -= 0x10000;
            output[out++] = static_cast<uint16_t>(0xD800 + (codepoint >> 10));
            output[out++] = static_cast<uint16_t>(0xDC00 + (codepoint & 0x3FF));
        }
    }
    output[out] = 0;
}

std::string utf16ToUtf8(const uint16_t* input) {
    std::string output;
    if (!input) return output;
    output.reserve(SCE_IME_DIALOG_MAX_TEXT_LENGTH);

    for (size_t i = 0; input[i] != 0; ++i) {
        uint32_t codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            const uint32_t low = input[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                codepoint = 0xFFFD;
            }
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }
        appendUtf8(output, codepoint);
    }
    return output;
}

void logImeResult(const char* marker, int32_t result) {
    char line[128];
    const int length = sceClibSnprintf(
        line, sizeof(line), "ime %s result=0x%08X\n",
        marker ? marker : "event", static_cast<unsigned>(result));
    if (length > 0) {
        vita::appendFile(
            "ux0:data/FlashVita/runtime.log",
            line,
            static_cast<size_t>(length));
    }
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>& bytes) {
    return vita::readFile(path, bytes) && !bytes.empty();
}

std::string gameIdFromPath(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t begin = slash == std::string::npos ? 0 : slash + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end < begin) end = path.size();

    std::string id;
    for (size_t i = begin; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        if (std::isalnum(c) || c == '-' || c == '_') {
            id.push_back(static_cast<char>(c));
        } else {
            id.push_back('_');
        }
    }
    if (id.empty()) return "game";

    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "ssf2" ||
        lower.find("super-smash-flash-2") != std::string::npos ||
        lower.find("super_smash_flash_2") != std::string::npos) {
        return "SSF2";
    }
    return id;
}

std::string trimText(std::string text) {
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' ||
            text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' ||
            text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    if (begin > 0) text.erase(0, begin);
    return text;
}

struct GameOrigins {
    std::string web;
    std::string air;
};

GameOrigins gameOriginsFor(const std::string& game_id,
                           const std::string& cache_root) {
    const std::string override_path = cache_root + "/origin.override.txt";
    std::string origin;
    if (vita::readTextFile(override_path, origin)) {
        origin = trimText(origin);
        if (!origin.empty()) return {origin, origin};
    }

    GameOrigins origins;
    origins.air = std::string("file:///FlashVita/") + game_id + ".swf";

    std::string lower = game_id;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "ssf2") {
        // Web SSF2 builds switch from local data/ paths to their embedded CDN
        // when LocalConnection.domain is a SuperSmashFlash domain.
        origins.web =
            "https://www.supersmashflash.com/games/super-smash-flash-2/1032/SSF2.swf";
    } else {
        origins.web = origins.air;
    }
    return origins;
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
    dst.is_air = (src.reserved & 1U) != 0;
    dst.frame_count = src.frame_count;
    dst.tag_count = src.tag_count;
    dst.stage_width = src.stage_width;
    dst.stage_height = src.stage_height;
    dst.frame_rate = src.frame_rate;
}

void logRendererStats(const char* reason) {
    if (!vita::perfLoggingEnabled()) return;
    FlashVitaRendererStats stats{};
    if (flashvita_ruffle_renderer_stats(&stats) != 0) return;
    static uint64_t previous_worker_clocks[2]{};
    flashvita::vita::WorkerRuntimeStats worker_stats{};
    flashvita::vita::getWorkerRuntimeStats(worker_stats);
    const uint64_t worker0_delta =
        worker_stats.run_clocks[0] >= previous_worker_clocks[0]
            ? worker_stats.run_clocks[0] - previous_worker_clocks[0]
            : 0;
    const uint64_t worker1_delta =
        worker_stats.run_clocks[1] >= previous_worker_clocks[1]
            ? worker_stats.run_clocks[1] - previous_worker_clocks[1]
            : 0;
    previous_worker_clocks[0] = worker_stats.run_clocks[0];
    previous_worker_clocks[1] = worker_stats.run_clocks[1];

    char line[768];
    const int length = sceClibSnprintf(
        line,
        sizeof(line),
        "ruffle_renderer reason=%s frames=%llu color=%llu textured=%llu uploads=%llu "
        "partial_uploads=%llu uploaded_bytes=%llu "
        "lines=%llu gradient_skip=%llu missing_bitmap=%llu mask_ops=%llu blends=%llu stage3d=%llu "
        "xform_vertices=%llu parallel_draws=%llu parallel_batches=%llu parallel_jobs=%llu "
        "color_submissions=%llu prepass_us=%llu submit_us=%llu "
        "worker_clk_delta=%llu,%llu worker_last_cpu=%d,%d\n",
        reason ? reason : "periodic",
        static_cast<unsigned long long>(stats.frames),
        static_cast<unsigned long long>(stats.colored_draws),
        static_cast<unsigned long long>(stats.textured_draws),
        static_cast<unsigned long long>(stats.bitmap_uploads),
        static_cast<unsigned long long>(stats.bitmap_partial_uploads),
        static_cast<unsigned long long>(stats.bitmap_uploaded_bytes),
        static_cast<unsigned long long>(stats.lines),
        static_cast<unsigned long long>(stats.gradient_skips),
        static_cast<unsigned long long>(stats.missing_bitmaps),
        static_cast<unsigned long long>(stats.mask_ops),
        static_cast<unsigned long long>(stats.blends),
        static_cast<unsigned long long>(stats.stage3d),
        static_cast<unsigned long long>(stats.transformed_vertices),
        static_cast<unsigned long long>(stats.parallel_draws),
        static_cast<unsigned long long>(stats.parallel_batches),
        static_cast<unsigned long long>(stats.parallel_jobs),
        static_cast<unsigned long long>(stats.color_submissions),
        static_cast<unsigned long long>(stats.prepass_us),
        static_cast<unsigned long long>(stats.submit_us),
        static_cast<unsigned long long>(worker0_delta),
        static_cast<unsigned long long>(worker1_delta),
        worker_stats.last_cpu[0],
        worker_stats.last_cpu[1]);
    if (length > 0) {
        vita::appendFile(
            "ux0:data/FlashVita/runtime.log",
            line,
            static_cast<size_t>(length));
    }
}

uint64_t visibleRendererDrawCount() {
    FlashVitaRendererStats stats{};
    if (flashvita_ruffle_renderer_stats(&stats) != 0) return 0;
    return stats.colored_draws + stats.textured_draws + stats.lines + stats.stage3d;
}

void logRuntimePerf(uint32_t ticks, uint32_t rendered,
                    uint64_t update_total_us, uint64_t update_max_us,
                    uint64_t render_total_us, uint64_t render_max_us) {
    if (ticks == 0 || !vita::perfLoggingEnabled()) return;
    const uint64_t total_us = update_total_us + render_total_us;
    const uint64_t max_total_us = update_max_us + render_max_us;
    char line[384];
    const int length = sceClibSnprintf(
        line,
        sizeof(line),
        "ruffle_perf ticks=%u rendered=%u avg_total_us=%llu approx_max_total_us=%llu "
        "avg_update_us=%llu max_update_us=%llu avg_render_us=%llu max_render_us=%llu "
        "budget60_us=16667\n",
        ticks,
        rendered,
        static_cast<unsigned long long>(total_us / ticks),
        static_cast<unsigned long long>(max_total_us),
        static_cast<unsigned long long>(update_total_us / ticks),
        static_cast<unsigned long long>(update_max_us),
        static_cast<unsigned long long>(render_total_us / ticks),
        static_cast<unsigned long long>(render_max_us));
    if (length > 0) {
        vita::appendFile(
            "ux0:data/FlashVita/runtime.log",
            line,
            static_cast<size_t>(length));
    }
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

uint64_t RuffleRuntime::visibleDrawCount() {
#if FLASHVITA_ENABLE_RUFFLE
    return visibleRendererDrawCount();
#else
    return 0;
#endif
}

void RuffleRuntime::updateVirtualKeyboard() {
#if FLASHVITA_ENABLE_RUFFLE
    if (!handle_) return;

    if (!ime_active_) {
        uint8_t initial_utf8[kImeUtf8Capacity]{};
        FlashVitaTextInputInfo info{};
        const int32_t request = flashvita_ruffle_text_input_info(
            handle_, &info, initial_utf8, sizeof(initial_utf8));
        if (request <= 0) return;

        if (!ime_module_loaded_) {
            const int32_t load_result = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
            if (load_result < 0) {
                logImeResult("module_load_failed", load_result);
                flashvita_ruffle_virtual_keyboard_ack(handle_);
                return;
            }
            ime_module_loaded_ = true;
        }

        utf8ToUtf16(initial_utf8, ime_initial_text_, kImeUtf16Capacity);
        std::memcpy(ime_input_text_, ime_initial_text_, sizeof(ime_initial_text_));

        SceImeDialogParam param;
        sceImeDialogParamInit(&param);
        param.inputMethod = 0;
        param.supportedLanguages = 0x000FFFFFULL;
        param.languagesForced = SCE_FALSE;
        param.type = SCE_IME_TYPE_DEFAULT;
        param.option = info.multiline ? SCE_IME_OPTION_MULTILINE : 0;
        param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
        param.textBoxMode = info.password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD
                                         : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
        param.title = kImeTitle;
        param.maxTextLength = std::min<uint32_t>(
            info.max_length == 0 ? SCE_IME_DIALOG_MAX_TEXT_LENGTH : info.max_length,
            SCE_IME_DIALOG_MAX_TEXT_LENGTH);
        param.initialText = ime_initial_text_;
        param.inputTextBuffer = ime_input_text_;
        param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;

        const int32_t init_result = sceImeDialogInit(&param);
        flashvita_ruffle_virtual_keyboard_ack(handle_);
        if (init_result < 0) {
            logImeResult("dialog_init_failed", init_result);
            return;
        }

        ime_active_ = true;
        logImeResult("dialog_open", init_result);
        return;
    }

    const SceCommonDialogStatus status = sceImeDialogGetStatus();
    if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) return;

    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult result{};
        const int32_t result_code = sceImeDialogGetResult(&result);
        if (result_code >= 0 &&
            result.result == SCE_COMMON_DIALOG_RESULT_OK &&
            result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            const std::string text = utf16ToUtf8(ime_input_text_);
            const int32_t commit_result = flashvita_ruffle_replace_focused_text(
                handle_, reinterpret_cast<const uint8_t*>(text.data()), text.size());
            logImeResult("commit", commit_result);
        } else {
            logImeResult("cancel", result_code);
        }
        const int32_t term_result = sceImeDialogTerm();
        if (term_result < 0) logImeResult("dialog_term_failed", term_result);
        ime_active_ = false;
    } else if (status == SCE_COMMON_DIALOG_STATUS_NONE) {
        ime_active_ = false;
    }
#endif
}

void RuffleRuntime::closeVirtualKeyboard() {
#if FLASHVITA_ENABLE_RUFFLE
    if (ime_active_) {
        sceImeDialogAbort();
        sceImeDialogTerm();
        ime_active_ = false;
    }
    if (handle_) flashvita_ruffle_virtual_keyboard_ack(handle_);
    if (ime_module_loaded_) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
        ime_module_loaded_ = false;
    }
    std::memset(ime_initial_text_, 0, sizeof(ime_initial_text_));
    std::memset(ime_input_text_, 0, sizeof(ime_input_text_));
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
    const std::string game_id = gameIdFromPath(path);
    const std::string cache_root =
        std::string("ux0:data/FlashVita/gamefiles/") + game_id;
    vita::makeDirectories(cache_root);
    const GameOrigins origins = gameOriginsFor(game_id, cache_root);

    FlashVitaRuffleProbe probe{};
    logMemoryStats("start_before");
    handle_ = flashvita_ruffle_headless_create(
        path.c_str(),
        origins.web.c_str(),
        origins.air.c_str(),
        cache_root.c_str(),
        &probe);
    out.available = true;
    if (!handle_) {
        out.message = "Ruffle: failed to construct headless Player";
        return false;
    }
    logMemoryStats("start_after");
    copyProbe(probe, out);
    const std::string& movie_url = out.is_air ? origins.air : origins.web;
    vita::writeTextFile(cache_root + "/origin.txt", movie_url + "\n");

    {
        char line[512];
        const int length = sceClibSnprintf(
            line,
            sizeof(line),
            "gamefiles id=%s root=%s origin=%s air=%d single_parse=1\n",
            game_id.c_str(),
            cache_root.c_str(),
            movie_url.c_str(),
            out.is_air ? 1 : 0);
        if (length > 0) {
            vita::appendFile(
                "ux0:data/FlashVita/runtime.log",
                line,
                static_cast<size_t>(length));
        }
    }
    out.message = "Ruffle Player constructed with FlashVita vitaGL renderer";
    tick_counter_ = 0;
    rendered_last_tick_ = false;
    perf_update_total_us_ = 0;
    perf_update_max_us_ = 0;
    perf_render_total_us_ = 0;
    perf_render_max_us_ = 0;
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
    updateVirtualKeyboard();
    const bool profile = vita::perfLoggingEnabled();

    uint64_t update_us = 0;
    uint64_t render_us = 0;
    int32_t result = 1;

    if (!ime_active_) {
        const uint64_t update_begin_us = profile ? sceKernelGetProcessTimeWide() : 0;
        result = flashvita_ruffle_headless_update(handle_, dt_ms);
        if (profile) update_us = sceKernelGetProcessTimeWide() - update_begin_us;
        if (result < 0) return false;
    }

    const uint64_t render_begin_us = profile ? sceKernelGetProcessTimeWide() : 0;
    result = flashvita_ruffle_headless_render(handle_);
    if (profile) render_us = sceKernelGetProcessTimeWide() - render_begin_us;
    if (result < 0) return false;

    rendered_last_tick_ = result > 0;
    ++tick_counter_;
    if (!profile) {
        perf_ticks_ = 0;
        perf_rendered_ = 0;
        perf_update_total_us_ = perf_update_max_us_ = 0;
        perf_render_total_us_ = perf_render_max_us_ = 0;
        return true;
    }
    perf_update_total_us_ += update_us;
    if (update_us > perf_update_max_us_) perf_update_max_us_ = update_us;
    perf_render_total_us_ += render_us;
    if (render_us > perf_render_max_us_) perf_render_max_us_ = render_us;
    ++perf_ticks_;
    if (rendered_last_tick_) ++perf_rendered_;
    if (perf_ticks_ >= 30) {
        logRuntimePerf(perf_ticks_, perf_rendered_,
                       perf_update_total_us_, perf_update_max_us_,
                       perf_render_total_us_, perf_render_max_us_);
        perf_update_total_us_ = 0;
        perf_update_max_us_ = 0;
        perf_render_total_us_ = 0;
        perf_render_max_us_ = 0;
        perf_ticks_ = 0;
        perf_rendered_ = 0;
    }
    if (tick_counter_ == 1 || (vita::perfLoggingEnabled() && (tick_counter_ % 30) == 0)) {
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

bool RuffleRuntime::mouseLeave() {
#if FLASHVITA_ENABLE_RUFFLE
    return handle_ && flashvita_ruffle_mouse_leave(handle_) == 0;
#else
    return false;
#endif
}

void RuffleRuntime::stop() {
#if FLASHVITA_ENABLE_RUFFLE
    closeVirtualKeyboard();
    if (handle_) {
        logMemoryStats("stop_before");
        // Ensure the GPU is no longer consuming resources owned by the current
        // player before its texture handles are destroyed.
        glFinish();
        flashvita_ruffle_headless_destroy(handle_);
        handle_ = nullptr;
        flashvita_vitagl_invalidate_cache();
        glFinish();

        const int trimmed = malloc_trim(0);
        char line[96];
        const int length = sceClibSnprintf(
            line,
            sizeof(line),
            "memory malloc_trim=%d\n",
            trimmed);
        if (length > 0) {
            vita::appendFile(
                "ux0:data/FlashVita/runtime.log",
                line,
                static_cast<size_t>(length));
        }
        logMemoryStats("stop_after");
    }
#endif
    handle_ = nullptr;
    tick_counter_ = 0;
    rendered_last_tick_ = false;
    perf_update_total_us_ = 0;
    perf_update_max_us_ = 0;
    perf_render_total_us_ = 0;
    perf_render_max_us_ = 0;
    perf_ticks_ = 0;
    perf_rendered_ = 0;
}

} // namespace flashvita
