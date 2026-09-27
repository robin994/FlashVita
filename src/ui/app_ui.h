#pragma once

#include "../config/app_config.h"
#include "../input/input_mapper.h"
#include "../library/swf_library.h"
#include "../player/flash_player.h"

#include <string>

namespace flashvita {

class AppUi {
public:
    AppUi(SwfLibrary& library, InputMapper& input, AppConfig& config, FlashPlayer& player);

    void draw();
    void applyTheme();
    bool takeLaunchRequest(std::string& path);
    bool launchLoading() const { return launch_loading_; }
    void setLaunchLoading(bool loading) { launch_loading_ = loading; }

private:
    enum class Screen {
        Library,
        Settings,
        Controls,
        Player
    };

    const SwfInfo* selectedGame() const;
    void drawTopBar();
    void drawLibrary();
    void drawSettings();
    void drawControls();
    void drawPlayer();
    void refreshLibrary();

    SwfLibrary& library_;
    InputMapper& input_;
    AppConfig& config_;
    FlashPlayer& player_;
    Screen screen_ = Screen::Library;
    int selected_index_ = -1;
    std::string notification_;
    bool launch_requested_ = false;
    bool launch_loading_ = false;
    bool launch_screen_presented_ = false;
    std::string pending_launch_path_;
    std::string pending_launch_name_;
};

} // namespace flashvita
