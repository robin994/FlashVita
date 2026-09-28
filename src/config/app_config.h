#pragma once

namespace flashvita {

struct AppConfig {
    bool vsync = true;
    bool show_invalid_swf = true;
    bool remember_last_game = true;
    bool enable_logs = false;
    bool enable_perf_logs = false;
    bool async_renderer = true;
    int ui_theme = 0;
    float ui_scale = 1.0f;

    void load();
    void save() const;
};

} // namespace flashvita
