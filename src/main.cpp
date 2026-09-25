#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include <imgui_vita.h>
#include <vitaGL.h>

#include <cstdio>

#include "config/app_config.h"
#include "input/input_mapper.h"
#include "input/runtime_input.h"
#include "library/swf_library.h"
#include "player/flash_player.h"
#include "ui/app_ui.h"

namespace {

void logMarker(const char* marker) {
    FILE* f = std::fopen("ux0:data/FlashVita/runtime.log", "a");
    if (!f) return;
    std::fprintf(f, "%s\n", marker);
    std::fclose(f);
}

void ensureDataDirectories() {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    sceIoMkdir("ux0:data/FlashVita/games", 0777);
    sceIoMkdir("ux0:data/FlashVita/profiles", 0777);
    sceIoMkdir("ux0:data/FlashVita/saves", 0777);
}

} // namespace

int main() {
    ensureDataDirectories();
    std::remove("ux0:data/FlashVita/runtime.log");
    logMarker("FLASHVITA_BOOT v0.8-pacing-ui-isolation");

    flashvita::AppConfig config;
    config.load();
    logMarker("config_loaded");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
    vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(config.vsync ? GL_TRUE : GL_FALSE);
    logMarker("vitagl_init_pass");

    glViewport(0, 0, 960, 544);
    glClearColor(0.055f, 0.065f, 0.085f, 1.0f);
    glDisable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    ImGui::CreateContext();
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF("app0:/Roboto_compact.ttf", 16.0f);
    logMarker(font ? "imgui_font_pass" : "imgui_font_fail");
    ImGui_ImplVitaGL_Init();
    ImGui_ImplVitaGL_TouchUsage(true);
    ImGui_ImplVitaGL_UseIndirectFrontTouch(true);
    ImGui_ImplVitaGL_GamepadUsage(true);
    ImGui_ImplVitaGL_MouseStickUsage(true);
    ImGui::GetIO().MouseDrawCursor = true;
    logMarker("imgui_backend_init_pass");

    flashvita::SwfLibrary library;
    library.scan("ux0:data/FlashVita/games");
    flashvita::InputMapper input;
    flashvita::RuntimeInput runtime_input;
    flashvita::FlashPlayer player;
    flashvita::AppUi ui(library, input, config, player);
    logMarker("ui_constructed");

    unsigned frame = 0;
    bool ui_visible = true;
    bool previous_toggle_chord = false;
    bool previous_player_running = false;
    uint64_t last_game_tick_us = 0;
    constexpr uint32_t kUiToggleMask = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    for (;;) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        vglWaitVblankStart(config.vsync ? GL_TRUE : GL_FALSE);

        SceCtrlData raw_pad{};
        sceCtrlPeekBufferPositive(0, &raw_pad, 1);
        const bool toggle_chord = (raw_pad.buttons & kUiToggleMask) == kUiToggleMask;

        const bool player_running = player.ruffleRunning();
        if (player_running && !previous_player_running) {
            ui_visible = false;
            last_game_tick_us = 0;
            logMarker("ui_hidden_game_started");
        } else if (!player_running && previous_player_running) {
            ui_visible = true;
            last_game_tick_us = 0;
            logMarker("ui_shown_game_stopped");
        }

        if (toggle_chord && !previous_toggle_chord && player_running) {
            ui_visible = !ui_visible;
            last_game_tick_us = 0;
            if (ui_visible) runtime_input.suspend(player, input.profile());
            logMarker(ui_visible ? "ui_toggle_shown" : "ui_toggle_hidden");
        }
        previous_toggle_chord = toggle_chord;
        previous_player_running = player_running;

        if (ui_visible) {
            flashvita::RuffleRuntime::prepareUiGraphics();
            ImGui_ImplVitaGL_NewFrame();
            ImGui::GetIO().MouseDrawCursor = true;
            glClear(GL_COLOR_BUFFER_BIT);
        } else if (!player_running) {
            glClear(GL_COLOR_BUFFER_BIT);
        }
        if (player_running && !ui_visible) {
            const uint64_t now_us = sceKernelGetProcessTimeWide();
            double dt_ms = last_game_tick_us == 0
                ? (1000.0 / 60.0)
                : static_cast<double>(now_us - last_game_tick_us) / 1000.0;
            last_game_tick_us = now_us;
            if (dt_ms < 1.0) dt_ms = 1.0;
            if (dt_ms > 100.0) dt_ms = 100.0;
            runtime_input.update(player, input.profile(), toggle_chord ? kUiToggleMask : 0);
            player.tick(dt_ms);
        }
        if (ui_visible) {
            ui.draw();
            glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x),
                       static_cast<int>(ImGui::GetIO().DisplaySize.y));
            ImGui::Render();
            if (frame == 0) {
                char marker[128];
                std::snprintf(marker, sizeof(marker), "first_frame cmd_lists=%d vertices=%d indices=%d",
                              ImGui::GetDrawData() ? ImGui::GetDrawData()->CmdListsCount : -1,
                              ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalVtxCount : -1,
                              ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalIdxCount : -1);
                logMarker(marker);
            }
            ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
            if (frame == 0) {
                char marker[64];
                std::snprintf(marker, sizeof(marker), "post_render gl_error=0x%04X",
                              static_cast<unsigned>(glGetError()));
                logMarker(marker);
            }
        }
        vglSwapBuffers(GL_FALSE);
        ++frame;
    }

    return 0;
}
