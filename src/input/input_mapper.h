#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace flashvita {

enum class VitaControl {
    Cross = 0,
    Circle,
    Square,
    Triangle,
    L,
    R,
    Start,
    Select,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count
};

enum class FlashKey {
    None = 0,
    Space,
    Enter,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Z,
    X,
    C,
    A,
    S,
    D,
    Q,
    W,
    E,
    Shift,
    Ctrl
};

struct InputProfile {
    std::array<FlashKey, static_cast<size_t>(VitaControl::Count)> keys;
    bool left_stick_mouse = true;
    bool front_touch_mouse = true;
    bool rear_touch_mouse = false;
    float mouse_speed = 1.0f;
};

class InputMapper {
public:
    InputMapper();

    InputProfile& profile() { return profile_; }
    const InputProfile& profile() const { return profile_; }
    void resetDefaults();
    bool loadForGame(const std::string& game_name);
    bool saveForGame(const std::string& game_name) const;

    static const char* controlName(VitaControl control);
    static const char* keyName(FlashKey key);
    static int keyCount();
    static FlashKey keyFromIndex(int index);
    static int keyToIndex(FlashKey key);

private:
    static std::string profilePath(const std::string& game_name);
    InputProfile profile_;
};

} // namespace flashvita
