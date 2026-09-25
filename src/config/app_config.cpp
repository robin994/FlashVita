#include "app_config.h"

#include <cstdio>
#include <psp2/io/stat.h>

namespace flashvita {

void AppConfig::load() {
    FILE* f = std::fopen("ux0:data/FlashVita/config.ini", "r");
    if (!f) return;
    char line[128];
    int value = 0;
    float fvalue = 0.0f;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "vsync=%d", &value) == 1) vsync = value != 0;
        else if (std::sscanf(line, "show_invalid_swf=%d", &value) == 1) show_invalid_swf = value != 0;
        else if (std::sscanf(line, "remember_last_game=%d", &value) == 1) remember_last_game = value != 0;
        else if (std::sscanf(line, "ui_theme=%d", &value) == 1) ui_theme = value;
        else if (std::sscanf(line, "ui_scale=%f", &fvalue) == 1) ui_scale = fvalue;
    }
    std::fclose(f);
}

void AppConfig::save() const {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    FILE* f = std::fopen("ux0:data/FlashVita/config.ini", "w");
    if (!f) return;
    std::fprintf(f, "vsync=%d\n", vsync ? 1 : 0);
    std::fprintf(f, "show_invalid_swf=%d\n", show_invalid_swf ? 1 : 0);
    std::fprintf(f, "remember_last_game=%d\n", remember_last_game ? 1 : 0);
    std::fprintf(f, "ui_theme=%d\n", ui_theme);
    std::fprintf(f, "ui_scale=%.2f\n", ui_scale);
    std::fclose(f);
}

} // namespace flashvita
