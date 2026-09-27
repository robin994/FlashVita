#include "app_ui.h"
#include "../version.h"
#include "../platform/vita_native.h"

#include <cstdio>
#include <imgui_vita.h>

namespace flashvita {
namespace {

const char* formatBytes(uint64_t bytes, char* out, size_t out_size) {
    static const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::snprintf(out, out_size, unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
    return out;
}

const char* compressionName(const SwfInfo& game) {
    if (!game.header_valid) return "Unknown / invalid header";
    if (game.lzma) return "ZWS / LZMA";
    if (game.compressed) return "CWS / zlib";
    return "FWS / uncompressed";
}

} // namespace

AppUi::AppUi(SwfLibrary& library, InputMapper& input, AppConfig& config, FlashPlayer& player)
    : library_(library), input_(input), config_(config), player_(player) {
    applyTheme();
}

void AppUi::applyTheme() {
    if (config_.ui_theme == 1) {
        ImGui::StyleColorsLight();
    } else {
        ImGui::StyleColorsDark();
        if (config_.ui_theme == 2) {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 0.0f;
            style.FrameRounding = 0.0f;
            style.GrabRounding = 0.0f;
        }
    }
    ImGui::GetIO().FontGlobalScale = config_.ui_scale;
}

const SwfInfo* AppUi::selectedGame() const {
    const auto& games = library_.games();
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(games.size())) return nullptr;
    return &games[static_cast<size_t>(selected_index_)];
}

void AppUi::refreshLibrary() {
    library_.scan("ux0:data/FlashVita/games");
    if (selected_index_ >= static_cast<int>(library_.games().size())) selected_index_ = -1;
    notification_ = "Library refreshed";
}

void AppUi::drawTopBar() {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(960.0f, 54.0f), ImGuiSetCond_Always);
    ImGui::Begin("##flashvita_top", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPos(ImVec2(12.0f, 7.0f));
    ImGui::Text("FlashVita");
    ImGui::SetCursorPos(ImVec2(12.0f, 27.0f));
    ImGui::TextDisabled("%s", FLASHVITA_VERSION_LABEL);

    ImGui::SetCursorPos(ImVec2(120.0f, 10.0f));
    if (ImGui::Button("Library", ImVec2(110.0f, 32.0f))) screen_ = Screen::Library;
    ImGui::SameLine();
    if (ImGui::Button("Controls", ImVec2(110.0f, 32.0f))) screen_ = Screen::Controls;
    ImGui::SameLine();
    if (ImGui::Button("Settings", ImVec2(110.0f, 32.0f))) screen_ = Screen::Settings;
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(110.0f, 32.0f))) refreshLibrary();

    if (!notification_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", notification_.c_str());
    }
    ImGui::End();
}

void AppUi::drawLibrary() {
    ImGui::SetNextWindowPos(ImVec2(12.0f, 66.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(440.0f, 466.0f), ImGuiSetCond_Always);
    ImGui::Begin("SWF Library", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    const auto& games = library_.games();
    if (games.empty()) {
        ImGui::TextWrapped("No SWF files found.");
        ImGui::Spacing();
        ImGui::TextWrapped("Copy games to:");
        ImGui::Text("ux0:data/FlashVita/games/");
        ImGui::Spacing();
        ImGui::TextDisabled("Press Refresh after copying files.");
    } else {
        for (size_t i = 0; i < games.size(); ++i) {
            const SwfInfo& game = games[i];
            if (!config_.show_invalid_swf && !game.header_valid) continue;
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##game%u", game.header_valid ? "" : "[?] ", game.name.c_str(), static_cast<unsigned>(i));
            if (ImGui::Selectable(label, selected_index_ == static_cast<int>(i), 0, ImVec2(0.0f, 34.0f))) {
                selected_index_ = static_cast<int>(i);
                input_.loadForGame(game.name);
                notification_.clear();
            }
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(464.0f, 66.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(484.0f, 466.0f), ImGuiSetCond_Always);
    ImGui::Begin("Game Details", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    const SwfInfo* game = selectedGame();
    if (!game) {
        ImGui::TextWrapped("Select a SWF from the library to inspect it, configure its controls, or open it in the player.");
    } else {
        char size_text[32];
        ImGui::TextWrapped("%s", game->name.c_str());
        ImGui::Separator();
        ImGui::Text("File size: %s", formatBytes(game->size, size_text, sizeof(size_text)));
        ImGui::Text("Header: %s", compressionName(*game));
        if (game->header_valid) {
            ImGui::Text("SWF version: %d", game->version);
            ImGui::Text("Declared size: %u bytes", game->declared_size);
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", game->path.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Play", ImVec2(150.0f, 40.0f))) {
            pending_launch_path_ = game->path;
            pending_launch_name_ = game->name;
            launch_requested_ = true;
            launch_loading_ = true;
            launch_screen_presented_ = false;
            screen_ = Screen::Player;
        }
        ImGui::SameLine();
        if (ImGui::Button("Map controls", ImVec2(150.0f, 40.0f))) {
            input_.loadForGame(game->name);
            screen_ = Screen::Controls;
        }
        ImGui::Spacing();
        ImGui::TextDisabled("M1: Play currently opens the runtime placeholder.");
    }
    ImGui::End();
}

void AppUi::drawSettings() {
    ImGui::SetNextWindowPos(ImVec2(80.0f, 82.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(800.0f, 430.0f), ImGuiSetCond_Always);
    ImGui::Begin("FlashVita Settings", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Interface");
    ImGui::Separator();
    const char* themes[] = {"Dark", "Light", "Classic"};
    int old_theme = config_.ui_theme;
    ImGui::Combo("Theme", &config_.ui_theme, themes, 3);
    if (old_theme != config_.ui_theme) applyTheme();

    float old_scale = config_.ui_scale;
    ImGui::SliderFloat("UI scale", &config_.ui_scale, 0.80f, 1.35f, "%.2f");
    if (old_scale != config_.ui_scale) applyTheme();

    ImGui::Checkbox("VSync", &config_.vsync);
    ImGui::Checkbox("Show SWF files with invalid/unknown headers", &config_.show_invalid_swf);
    ImGui::Checkbox("Remember last game (reserved for runtime milestone)", &config_.remember_last_game);
    const bool old_logs = config_.enable_logs;
    ImGui::Checkbox("Enable runtime logs", &config_.enable_logs);
    if (old_logs != config_.enable_logs) {
        vita::setLoggingEnabled(config_.enable_logs);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Disable for lower I/O overhead and better runtime performance");

    ImGui::Spacing();
    ImGui::Text("Storage");
    ImGui::Separator();
    ImGui::Text("Games:    ux0:data/FlashVita/games/");
    ImGui::Text("Profiles: ux0:data/FlashVita/profiles/");
    ImGui::Text("Config:   ux0:data/FlashVita/config.ini");

    ImGui::Spacing();
    if (ImGui::Button("Save settings", ImVec2(180.0f, 36.0f))) {
        config_.save();
        notification_ = "Settings saved";
    }
    ImGui::SameLine();
    if (ImGui::Button("Back to library", ImVec2(180.0f, 36.0f))) screen_ = Screen::Library;
    ImGui::End();
}

void AppUi::drawControls() {
    ImGui::SetNextWindowPos(ImVec2(56.0f, 70.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(848.0f, 458.0f), ImGuiSetCond_Always);
    ImGui::Begin("Controller Mapping", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    const SwfInfo* game = selectedGame();
    if (!game) {
        ImGui::TextWrapped("Select a game in Library first. Control profiles are stored per SWF.");
        ImGui::Spacing();
        if (ImGui::Button("Back to library")) screen_ = Screen::Library;
        ImGui::End();
        return;
    }

    ImGui::Text("Profile: %s", game->name.c_str());
    ImGui::Separator();

    InputProfile& profile = input_.profile();
    const int total = static_cast<int>(VitaControl::Count);
    for (int i = 0; i < total; ++i) {
        const VitaControl control = static_cast<VitaControl>(i);
        ImGui::PushID(i);
        ImGui::Text("%-12s", InputMapper::controlName(control));
        ImGui::SameLine(185.0f);
        int current = InputMapper::keyToIndex(profile.keys[static_cast<size_t>(i)]);
        if (ImGui::BeginCombo("##mapping", InputMapper::keyName(profile.keys[static_cast<size_t>(i)]))) {
            for (int k = 0; k < InputMapper::keyCount(); ++k) {
                const FlashKey key = InputMapper::keyFromIndex(k);
                const bool selected = current == k;
                if (ImGui::Selectable(InputMapper::keyName(key), selected))
                    profile.keys[static_cast<size_t>(i)] = key;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    ImGui::SetCursorPos(ImVec2(470.0f, 70.0f));
    ImGui::BeginGroup();
    ImGui::Text("Mouse emulation");
    ImGui::Separator();
    ImGui::Checkbox("Left stick controls mouse", &profile.left_stick_mouse);
    ImGui::Checkbox("Front touch controls mouse", &profile.front_touch_mouse);
    ImGui::Checkbox("Rear touch controls mouse", &profile.rear_touch_mouse);
    ImGui::Checkbox("Cross clicks / drags mouse", &profile.cross_mouse_click);
    ImGui::SliderFloat("Mouse speed", &profile.mouse_speed, 0.25f, 3.0f, "%.2fx");
    ImGui::Spacing();
    ImGui::TextWrapped("These mappings will be translated to Flash keyboard/mouse events once the runtime is connected.");
    ImGui::EndGroup();

    ImGui::SetCursorPos(ImVec2(470.0f, 330.0f));
    if (ImGui::Button("Save profile", ImVec2(155.0f, 38.0f))) {
        notification_ = input_.saveForGame(game->name) ? "Control profile saved" : "Failed to save profile";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset defaults", ImVec2(155.0f, 38.0f))) {
        input_.resetDefaults();
        notification_ = "Default mapping restored";
    }
    ImGui::SetCursorPos(ImVec2(470.0f, 378.0f));
    if (ImGui::Button("Back to library", ImVec2(320.0f, 36.0f))) screen_ = Screen::Library;

    ImGui::End();
}

void AppUi::drawPlayer() {
    ImGui::SetNextWindowPos(ImVec2(30.0f, 70.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0f, 450.0f), ImGuiSetCond_Always);
    ImGui::Begin("Flash Player", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    if (launch_loading_) {
        launch_screen_presented_ = true;
        ImGui::SetCursorPosY(120.0f);
        ImGui::SetWindowFontScale(1.35f);
        ImGui::Text("Loading SWF...");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        if (!pending_launch_name_.empty()) {
            ImGui::TextWrapped("%s", pending_launch_name_.c_str());
        }
        ImGui::Spacing();
        ImGui::TextWrapped("Parsing, decompressing and initializing ActionScript. Large SWFs can spend several seconds in first-frame startup on PS Vita.");
        ImGui::Spacing();
        ImGui::TextDisabled("Waiting for the first visible frame...");
        ImGui::Spacing();
        if (ImGui::Button("Back to library", ImVec2(180.0f, 40.0f))) {
            player_.close();
            launch_requested_ = false;
            launch_loading_ = false;
            launch_screen_presented_ = false;
            screen_ = Screen::Library;
        }
        ImGui::End();
        return;
    }

    ImGui::Text("Runtime bring-up placeholder");
    ImGui::Separator();
    ImGui::TextWrapped("%s", player_.path().c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", player_.status().c_str());
    ImGui::Spacing();

    const SwfDocumentInfo& doc = player_.document();
    if (doc.valid) {
        ImGui::Columns(2, "swf_metadata", false);
        ImGui::Text("Stage");
        ImGui::NextColumn();
        ImGui::Text("%d x %d", doc.width, doc.height);
        ImGui::NextColumn();
        ImGui::Text("Frame rate");
        ImGui::NextColumn();
        ImGui::Text("%.2f FPS", doc.frame_rate);
        ImGui::NextColumn();
        ImGui::Text("Frames");
        ImGui::NextColumn();
        ImGui::Text("%u", static_cast<unsigned>(doc.frame_count));
        ImGui::NextColumn();
        ImGui::Text("Tags");
        ImGui::NextColumn();
        ImGui::Text("%u", doc.tag_count);
        ImGui::NextColumn();
        ImGui::Text("ShowFrame tags");
        ImGui::NextColumn();
        ImGui::Text("%u", doc.show_frame_tags);
        ImGui::NextColumn();
        ImGui::Text("AVM1 action tags");
        ImGui::NextColumn();
        ImGui::Text("%u (%u records)", doc.do_action_tags + doc.do_init_action_tags, doc.avm1_action_records);
        ImGui::NextColumn();
        ImGui::Text("DoABC / AVM2 tags");
        ImGui::NextColumn();
        ImGui::Text("%u", doc.do_abc_tags);
        ImGui::Columns(1);
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Text("Ruffle integration");
    ImGui::Text("Bridge: %s", RuffleRuntime::bridgeVersion());
    const RuffleProbeInfo& ruffle = player_.ruffleInfo();
    if (!RuffleRuntime::compiledIn()) {
        ImGui::TextDisabled("Rust bridge prepared; this VPK was built without the Ruffle static library.");
    } else if (ruffle.parsed) {
        ImGui::Text("Headless runtime: %s", player_.ruffleRunning() ? "RUNNING" : "stopped");
        ImGui::Text("Ruffle SWF: v%u, %dx%d, %.2f FPS, %u frames",
                    static_cast<unsigned>(ruffle.version),
                    ruffle.stage_width,
                    ruffle.stage_height,
                    ruffle.frame_rate,
                    static_cast<unsigned>(ruffle.frame_count));
        ImGui::Text("Ruffle tags: %u | AVM1: %s | AVM2: %s",
                    ruffle.tag_count,
                    ruffle.has_avm1 ? "yes" : "no",
                    ruffle.has_avm2 ? "yes" : "no");
    } else if (!ruffle.message.empty()) {
        ImGui::TextWrapped("%s", ruffle.message.c_str());
    }

    ImGui::TextWrapped("Ruffle owns AVM1/AVM2, timeline execution and Vita-native threaded audio. Remaining renderer work includes bitmap coverage, gradients, masks, text and persistent SharedObject storage.");
    ImGui::Spacing();
    if (ImGui::Button("Close player", ImVec2(180.0f, 40.0f))) {
        player_.close();
        screen_ = Screen::Library;
    }
    ImGui::End();
}

bool AppUi::takeLaunchRequest(std::string& path) {
    if (!launch_requested_ || !launch_screen_presented_) return false;
    path = pending_launch_path_;
    launch_requested_ = false;
    return !path.empty();
}

void AppUi::draw() {
    drawTopBar();
    switch (screen_) {
        case Screen::Library: drawLibrary(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Controls: drawControls(); break;
        case Screen::Player: drawPlayer(); break;
    }
}

} // namespace flashvita
