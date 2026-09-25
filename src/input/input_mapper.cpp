#include "input_mapper.h"

#include <cstdio>
#include <psp2/io/stat.h>

namespace flashvita {
namespace {

const FlashKey kKeys[] = {
    FlashKey::None, FlashKey::Space, FlashKey::Enter, FlashKey::Escape,
    FlashKey::Up, FlashKey::Down, FlashKey::Left, FlashKey::Right,
    FlashKey::Z, FlashKey::X, FlashKey::C, FlashKey::A, FlashKey::S,
    FlashKey::D, FlashKey::Q, FlashKey::W, FlashKey::E, FlashKey::Shift,
    FlashKey::Ctrl
};

std::string sanitize(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    return out;
}

} // namespace

InputMapper::InputMapper() { resetDefaults(); }

void InputMapper::resetDefaults() {
    profile_.keys.fill(FlashKey::None);
    profile_.keys[static_cast<size_t>(VitaControl::Cross)] = FlashKey::Space;
    profile_.keys[static_cast<size_t>(VitaControl::Circle)] = FlashKey::X;
    profile_.keys[static_cast<size_t>(VitaControl::Square)] = FlashKey::Z;
    profile_.keys[static_cast<size_t>(VitaControl::Triangle)] = FlashKey::C;
    profile_.keys[static_cast<size_t>(VitaControl::L)] = FlashKey::A;
    profile_.keys[static_cast<size_t>(VitaControl::R)] = FlashKey::S;
    profile_.keys[static_cast<size_t>(VitaControl::Start)] = FlashKey::Enter;
    profile_.keys[static_cast<size_t>(VitaControl::Select)] = FlashKey::Escape;
    profile_.keys[static_cast<size_t>(VitaControl::DpadUp)] = FlashKey::Up;
    profile_.keys[static_cast<size_t>(VitaControl::DpadDown)] = FlashKey::Down;
    profile_.keys[static_cast<size_t>(VitaControl::DpadLeft)] = FlashKey::Left;
    profile_.keys[static_cast<size_t>(VitaControl::DpadRight)] = FlashKey::Right;
    profile_.left_stick_mouse = true;
    profile_.front_touch_mouse = true;
    profile_.rear_touch_mouse = false;
    profile_.mouse_speed = 1.0f;
}

std::string InputMapper::profilePath(const std::string& game_name) {
    return std::string("ux0:data/FlashVita/profiles/") + sanitize(game_name) + ".ini";
}

bool InputMapper::saveForGame(const std::string& game_name) const {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    sceIoMkdir("ux0:data/FlashVita/profiles", 0777);
    FILE* f = std::fopen(profilePath(game_name).c_str(), "w");
    if (!f) return false;
    for (size_t i = 0; i < profile_.keys.size(); ++i)
        std::fprintf(f, "key_%u=%d\n", static_cast<unsigned>(i), static_cast<int>(profile_.keys[i]));
    std::fprintf(f, "left_stick_mouse=%d\n", profile_.left_stick_mouse ? 1 : 0);
    std::fprintf(f, "front_touch_mouse=%d\n", profile_.front_touch_mouse ? 1 : 0);
    std::fprintf(f, "rear_touch_mouse=%d\n", profile_.rear_touch_mouse ? 1 : 0);
    std::fprintf(f, "mouse_speed=%.3f\n", profile_.mouse_speed);
    std::fclose(f);
    return true;
}

bool InputMapper::loadForGame(const std::string& game_name) {
    resetDefaults();
    FILE* f = std::fopen(profilePath(game_name).c_str(), "r");
    if (!f) return false;

    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned idx = 0;
        int value = 0;
        float fvalue = 0.0f;
        if (std::sscanf(line, "key_%u=%d", &idx, &value) == 2 && idx < profile_.keys.size()) {
            profile_.keys[idx] = static_cast<FlashKey>(value);
        } else if (std::sscanf(line, "left_stick_mouse=%d", &value) == 1) {
            profile_.left_stick_mouse = value != 0;
        } else if (std::sscanf(line, "front_touch_mouse=%d", &value) == 1) {
            profile_.front_touch_mouse = value != 0;
        } else if (std::sscanf(line, "rear_touch_mouse=%d", &value) == 1) {
            profile_.rear_touch_mouse = value != 0;
        } else if (std::sscanf(line, "mouse_speed=%f", &fvalue) == 1) {
            profile_.mouse_speed = fvalue;
        }
    }
    std::fclose(f);
    return true;
}

const char* InputMapper::controlName(VitaControl control) {
    static const char* names[] = {
        "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"
    };
    return names[static_cast<size_t>(control)];
}

const char* InputMapper::keyName(FlashKey key) {
    switch (key) {
        case FlashKey::None: return "None";
        case FlashKey::Space: return "Space";
        case FlashKey::Enter: return "Enter";
        case FlashKey::Escape: return "Escape";
        case FlashKey::Up: return "Arrow Up";
        case FlashKey::Down: return "Arrow Down";
        case FlashKey::Left: return "Arrow Left";
        case FlashKey::Right: return "Arrow Right";
        case FlashKey::Z: return "Z";
        case FlashKey::X: return "X";
        case FlashKey::C: return "C";
        case FlashKey::A: return "A";
        case FlashKey::S: return "S";
        case FlashKey::D: return "D";
        case FlashKey::Q: return "Q";
        case FlashKey::W: return "W";
        case FlashKey::E: return "E";
        case FlashKey::Shift: return "Shift";
        case FlashKey::Ctrl: return "Ctrl";
    }
    return "Unknown";
}

int InputMapper::keyCount() { return static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0])); }
FlashKey InputMapper::keyFromIndex(int index) { return (index >= 0 && index < keyCount()) ? kKeys[index] : FlashKey::None; }
int InputMapper::keyToIndex(FlashKey key) {
    for (int i = 0; i < keyCount(); ++i) if (kKeys[i] == key) return i;
    return 0;
}

} // namespace flashvita
