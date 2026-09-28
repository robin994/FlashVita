#pragma once

#include <cstdint>
#include <string>

namespace flashvita {

struct RuffleProbeInfo {
    bool available = false;
    bool parsed = false;
    uint8_t version = 0;
    uint8_t compression = 0;
    bool has_avm1 = false;
    bool has_avm2 = false;
    bool is_air = false;
    uint16_t frame_count = 0;
    uint32_t tag_count = 0;
    int32_t stage_width = 0;
    int32_t stage_height = 0;
    float frame_rate = 0.0f;
    std::string message;
};

class RuffleRuntime {
public:
    RuffleRuntime() = default;
    ~RuffleRuntime();

    RuffleRuntime(const RuffleRuntime&) = delete;
    RuffleRuntime& operator=(const RuffleRuntime&) = delete;

    static bool compiledIn();
    static const char* bridgeVersion();
    static void prepareUiGraphics();
    static uint64_t visibleDrawCount();

    bool probeFile(const std::string& path, RuffleProbeInfo& out);
    bool startHeadless(const std::string& path, RuffleProbeInfo& out);
    bool tick(double dt_ms);
    bool renderNow();
    bool keyEvent(int key, bool down);
    bool mouseMove(double x, double y);
    bool mouseButton(double x, double y, bool down);
    bool mouseLeave();
    bool virtualKeyboardActive() const { return ime_active_; }
    void stop();
    bool running() const { return handle_ != nullptr; }
    bool renderedLastTick() const { return rendered_last_tick_; }

private:
    void updateVirtualKeyboard();
    void closeVirtualKeyboard();

    void* handle_ = nullptr;
    uint32_t tick_counter_ = 0;
    bool rendered_last_tick_ = false;
    uint64_t perf_update_total_us_ = 0;
    uint64_t perf_update_max_us_ = 0;
    uint64_t perf_render_total_us_ = 0;
    uint64_t perf_render_max_us_ = 0;
    uint32_t perf_ticks_ = 0;
    uint32_t perf_rendered_ = 0;
    bool ime_active_ = false;
    bool ime_module_loaded_ = false;
    uint16_t ime_initial_text_[2049]{};
    uint16_t ime_input_text_[2049]{};
};

} // namespace flashvita
