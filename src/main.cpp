#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>

#include <imgui_vita.h>
#include <vitaGL.h>

#include <algorithm>

#include "config/app_config.h"
#include "input/input_mapper.h"
#include "input/runtime_input.h"
#include "library/swf_library.h"
#include "platform/vita_native.h"
#include "player/flash_player.h"
#include "ui/app_ui.h"
#include "version.h"

extern "C" {
// Ruffle/AVM2 needs substantially more than VitaSDK's default 128 MB heap for
// large AS3 games. Reserve the larger heap before main; vitaGL will size its
// own RAM pool from the memory that remains.
unsigned int _newlib_heap_size_user = 192u * 1024u * 1024u;
unsigned int _get_vita_heap_size(void);
}

namespace {

void logMarker(const char* marker) {
    if (!marker || !flashvita::vita::loggingEnabled()) return;
    flashvita::vita::appendTextFile(
        "ux0:data/FlashVita/runtime.log",
        std::string(marker) + "\n");
}

void ensureDataDirectories() {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    sceIoMkdir("ux0:data/FlashVita/games", 0777);
    sceIoMkdir("ux0:data/FlashVita/profiles", 0777);
    sceIoMkdir("ux0:data/FlashVita/saves", 0777);
}

void requestGameClocks() {
    const int arm_result = scePowerSetArmClockFrequency(444);
    const int bus_result = scePowerSetBusClockFrequency(222);
    const int gpu_result = scePowerSetGpuClockFrequency(222);
    const int xbar_result = scePowerSetGpuXbarClockFrequency(166);

    char marker[192];
    sceClibSnprintf(marker, sizeof(marker),
                    "game_clocks arm=%d bus=%d gpu=%d xbar=%d result=%d,%d,%d,%d",
                    scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                    scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
                    arm_result, bus_result, gpu_result, xbar_result);
    logMarker(marker);
}

void drawRuntimeCursor(float x, float y) {
    const GLfloat vertices[] = {
        x, y,
        x + 5.0f, y + 17.0f,
        x + 10.0f, y + 10.0f,
    };

    glViewport(0, 0, 960, 544);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vertices);

    glColor4ub(255, 255, 255, 240);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glColor4ub(0, 0, 0, 255);
    glDrawArrays(GL_LINE_LOOP, 0, 3);
}

} // namespace

int main() {
    ensureDataDirectories();
    flashvita::AppConfig config;
    config.load();
    flashvita::vita::setLoggingEnabled(config.enable_logs);
    flashvita::vita::setPerfLoggingEnabled(config.enable_perf_logs);
    sceIoRemove("ux0:data/FlashVita/runtime.log");
    logMarker("pre_vita_native_init");
    const bool native_platform_ok = flashvita::vita::initialize();
    logMarker("FLASHVITA_BOOT " FLASHVITA_VERSION_LABEL "-" FLASHVITA_BUILD_TAG);
    logMarker(native_platform_ok ? "vita_native_init_pass" : "vita_native_init_fail");
    {
        char marker[96];
        sceClibSnprintf(marker, sizeof(marker), "newlib_heap_size=%u",
                        static_cast<unsigned>(_get_vita_heap_size()));
        logMarker(marker);
    }

    logMarker("config_loaded");
    requestGameClocks();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
    vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(config.vsync ? GL_TRUE : GL_FALSE);
    bool applied_vsync = config.vsync;
    logMarker("vitagl_init_pass");
    flashvita_vita_rust_allocator_enable_vgl();
    {
        char marker[160];
        sceClibSnprintf(marker, sizeof(marker),
                        "vitagl_ram_pool total=%llu free=%llu",
                        static_cast<unsigned long long>(vglMemTotal(VGL_MEM_RAM)),
                        static_cast<unsigned long long>(vglMemFree(VGL_MEM_RAM)));
        logMarker(marker);
    }

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
    uint64_t launch_visible_draw_baseline = 0;
    uint64_t last_game_tick_us = 0;
    uint32_t perf_game_frames = 0;
    uint64_t perf_loop_total_us = 0;
    uint64_t perf_loop_max_us = 0;
    uint64_t perf_game_total_us = 0;
    uint64_t perf_game_max_us = 0;
    uint64_t perf_swap_total_us = 0;
    uint64_t perf_swap_max_us = 0;
    constexpr uint32_t kUiToggleMask = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    for (;;) {
        bool loading_ready_to_show_game = false;
        const bool profile_frame = flashvita::vita::perfLoggingEnabled();
        const uint64_t loop_begin_us = profile_frame ? sceKernelGetProcessTimeWide() : 0;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (applied_vsync != config.vsync) {
            vglWaitVblankStart(config.vsync ? GL_TRUE : GL_FALSE);
            applied_vsync = config.vsync;
        }
        uint64_t game_work_us = 0;

        SceCtrlData raw_pad{};
        sceCtrlPeekBufferPositive(0, &raw_pad, 1);
        const bool toggle_chord = (raw_pad.buttons & kUiToggleMask) == kUiToggleMask;

        const bool player_running = player.ruffleRunning();
        if (player_running && !previous_player_running) {
            ui_visible = ui.launchLoading();
            last_game_tick_us = 0;
            logMarker(ui.launchLoading() ? "ui_loading_game_started" : "ui_hidden_game_started");
        } else if (!player_running && previous_player_running) {
            ui_visible = true;
            last_game_tick_us = 0;
            logMarker("ui_shown_game_stopped");
        }

        if (toggle_chord && !previous_toggle_chord && player_running && !ui.launchLoading() &&
            !player.virtualKeyboardActive()) {
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
        if (player_running && (!ui_visible || ui.launchLoading())) {
            const bool measure_game = profile_frame || ui.launchLoading();
            const uint64_t game_begin_us = measure_game ? sceKernelGetProcessTimeWide() : 0;
            const uint64_t now_us = sceKernelGetProcessTimeWide();
            double dt_ms = last_game_tick_us == 0
                ? (1000.0 / 60.0)
                : static_cast<double>(now_us - last_game_tick_us) / 1000.0;
            last_game_tick_us = now_us;
            if (dt_ms < 1.0) dt_ms = 1.0;
            if (dt_ms > 100.0) dt_ms = 100.0;
            if (!ui.launchLoading() && !player.virtualKeyboardActive()) {
                runtime_input.update(player, input.profile(), toggle_chord ? kUiToggleMask : 0);
            }
            player.tick(dt_ms);
            if (ui.launchLoading() &&
                flashvita::RuffleRuntime::visibleDrawCount() > launch_visible_draw_baseline) {
                loading_ready_to_show_game = true;
            }
            if (!ui.launchLoading() && !player.virtualKeyboardActive() && runtime_input.cursorVisible()) {
                drawRuntimeCursor(runtime_input.mouseX(), runtime_input.mouseY());
            }
            if (measure_game) game_work_us = sceKernelGetProcessTimeWide() - game_begin_us;
            if (ui.launchLoading() && game_work_us >= 500000) {
                char marker[96];
                sceClibSnprintf(marker, sizeof(marker), "loading_tick_us=%llu",
                                static_cast<unsigned long long>(game_work_us));
                logMarker(marker);
            }
        }
        if (ui_visible) {
            ui.draw();
            if (player_running && ui.launchLoading()) {
                flashvita::RuffleRuntime::prepareUiGraphics();
            }
            glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x),
                       static_cast<int>(ImGui::GetIO().DisplaySize.y));
            ImGui::Render();
            if (frame == 0) {
                char marker[128];
                sceClibSnprintf(marker, sizeof(marker), "first_frame cmd_lists=%d vertices=%d indices=%d",
                                ImGui::GetDrawData() ? ImGui::GetDrawData()->CmdListsCount : -1,
                                ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalVtxCount : -1,
                                ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalIdxCount : -1);
                logMarker(marker);
            }
            ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
            if (frame == 0) {
                char marker[64];
                sceClibSnprintf(marker, sizeof(marker), "post_render gl_error=0x%04X",
                                static_cast<unsigned>(glGetError()));
                logMarker(marker);
            }
        }
        const uint64_t swap_begin_us = profile_frame ? sceKernelGetProcessTimeWide() : 0;
        vglSwapBuffers(player.virtualKeyboardActive() ? GL_TRUE : GL_FALSE);
        const uint64_t swap_us = profile_frame ? sceKernelGetProcessTimeWide() - swap_begin_us : 0;

        if (loading_ready_to_show_game) {
            ui.setLaunchLoading(false);
            ui_visible = false;
            last_game_tick_us = 0;
            logMarker("loading_first_visible_frame");
        }

        std::string launch_path;
        if (ui.takeLaunchRequest(launch_path)) {
            launch_visible_draw_baseline = flashvita::RuffleRuntime::visibleDrawCount();
            logMarker("loading_ui_presented");
            if (player.open(launch_path)) {
                logMarker("loading_runtime_started");
            } else {
                ui.setLaunchLoading(false);
                ui_visible = true;
                logMarker("loading_runtime_failed");
            }
        }

        if (profile_frame && player_running && !ui_visible) {
            const uint64_t loop_us = sceKernelGetProcessTimeWide() - loop_begin_us;
            perf_loop_total_us += loop_us;
            perf_loop_max_us = std::max(perf_loop_max_us, loop_us);
            perf_game_total_us += game_work_us;
            perf_game_max_us = std::max(perf_game_max_us, game_work_us);
            perf_swap_total_us += swap_us;
            perf_swap_max_us = std::max(perf_swap_max_us, swap_us);
            ++perf_game_frames;

            if (perf_game_frames >= 30) {
                char marker[320];
                sceClibSnprintf(
                    marker,
                    sizeof(marker),
                    "frame_perf frames=%u avg_loop_us=%llu max_loop_us=%llu "
                    "avg_game_us=%llu max_game_us=%llu "
                    "avg_swap_us=%llu max_swap_us=%llu",
                    perf_game_frames,
                    static_cast<unsigned long long>(perf_loop_total_us / perf_game_frames),
                    static_cast<unsigned long long>(perf_loop_max_us),
                    static_cast<unsigned long long>(perf_game_total_us / perf_game_frames),
                    static_cast<unsigned long long>(perf_game_max_us),
                    static_cast<unsigned long long>(perf_swap_total_us / perf_game_frames),
                    static_cast<unsigned long long>(perf_swap_max_us));
                logMarker(marker);
                perf_game_frames = 0;
                perf_loop_total_us = 0;
                perf_loop_max_us = 0;
                perf_game_total_us = 0;
                perf_game_max_us = 0;
                perf_swap_total_us = 0;
                perf_swap_max_us = 0;
            }
        } else {
            perf_game_frames = 0;
            perf_loop_total_us = perf_loop_max_us = 0;
            perf_game_total_us = perf_game_max_us = 0;
            perf_swap_total_us = perf_swap_max_us = 0;
        }
        ++frame;
    }

    return 0;
}
