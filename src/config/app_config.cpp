#include "app_config.h"

#include <cstdio>
#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>

#include "../platform/vita_native.h"

namespace flashvita {

void AppConfig::load() {
    std::string text;
    if (!vita::readTextFile("ux0:data/FlashVita/config.ini", text)) return;

    int value = 0;
    float fvalue = 0.0f;
    size_t offset = 0;
    while (offset < text.size()) {
        const size_t end = text.find('\n', offset);
        const std::string line = text.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
        if (std::sscanf(line.c_str(), "vsync=%d", &value) == 1) vsync = value != 0;
        else if (std::sscanf(line.c_str(), "show_invalid_swf=%d", &value) == 1) show_invalid_swf = value != 0;
        else if (std::sscanf(line.c_str(), "remember_last_game=%d", &value) == 1) remember_last_game = value != 0;
        else if (std::sscanf(line.c_str(), "enable_logs=%d", &value) == 1) enable_logs = value != 0;
        else if (std::sscanf(line.c_str(), "ui_theme=%d", &value) == 1) ui_theme = value;
        else if (std::sscanf(line.c_str(), "ui_scale=%f", &fvalue) == 1) ui_scale = fvalue;
        if (end == std::string::npos) break;
        offset = end + 1;
    }
}

void AppConfig::save() const {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    char text[256];
    const int length = sceClibSnprintf(
        text,
        sizeof(text),
        "vsync=%d\nshow_invalid_swf=%d\nremember_last_game=%d\nenable_logs=%d\nui_theme=%d\nui_scale=%.2f\n",
        vsync ? 1 : 0,
        show_invalid_swf ? 1 : 0,
        remember_last_game ? 1 : 0,
        enable_logs ? 1 : 0,
        ui_theme,
        ui_scale);
    if (length > 0) {
        vita::writeFile("ux0:data/FlashVita/config.ini", text, static_cast<size_t>(length));
    }
}

} // namespace flashvita
