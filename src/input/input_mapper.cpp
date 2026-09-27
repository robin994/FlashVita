#include "input_mapper.h"

#include <cstdio>
#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>

#include "../platform/vita_native.h"

namespace flashvita {
namespace {

const FlashKey kKeys[] = {
    FlashKey::None, FlashKey::Space, FlashKey::Enter, FlashKey::Escape,
    FlashKey::Up, FlashKey::Down, FlashKey::Left, FlashKey::Right,
    FlashKey::Z, FlashKey::X, FlashKey::C, FlashKey::A, FlashKey::S,
    FlashKey::D, FlashKey::Q, FlashKey::W, FlashKey::E, FlashKey::Shift,
    FlashKey::Ctrl, FlashKey::O, FlashKey::P, FlashKey::Backspace
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
    profile_.keys[static_cast<size_t>(VitaControl::Cross)] = FlashKey::O;
    profile_.keys[static_cast<size_t>(VitaControl::Circle)] = FlashKey::X;
    profile_.keys[static_cast<size_t>(VitaControl::Square)] = FlashKey::P;
    profile_.keys[static_cast<size_t>(VitaControl::Triangle)] = FlashKey::C;
    profile_.keys[static_cast<size_t>(VitaControl::L)] = FlashKey::A;
    profile_.keys[static_cast<size_t>(VitaControl::R)] = FlashKey::S;
    profile_.keys[static_cast<size_t>(VitaControl::Start)] = FlashKey::Enter;
    profile_.keys[static_cast<size_t>(VitaControl::Select)] = FlashKey::Backspace;
    profile_.keys[static_cast<size_t>(VitaControl::DpadUp)] = FlashKey::Up;
    profile_.keys[static_cast<size_t>(VitaControl::DpadDown)] = FlashKey::Down;
    profile_.keys[static_cast<size_t>(VitaControl::DpadLeft)] = FlashKey::Left;
    profile_.keys[static_cast<size_t>(VitaControl::DpadRight)] = FlashKey::Right;
    profile_.left_stick_mouse = true;
    profile_.front_touch_mouse = true;
    profile_.rear_touch_mouse = false;
    profile_.cross_mouse_click = true;
    profile_.mouse_speed = 1.0f;
}

std::string InputMapper::profilePath(const std::string& game_name) {
    return std::string("ux0:data/FlashVita/profiles/") + sanitize(game_name) + ".ini";
}

bool InputMapper::saveForGame(const std::string& game_name) const {
    sceIoMkdir("ux0:data/FlashVita", 0777);
    sceIoMkdir("ux0:data/FlashVita/profiles", 0777);
    std::string text = "profile_version=2\n";
    char line[128];
    for (size_t i = 0; i < profile_.keys.size(); ++i) {
        const int length = sceClibSnprintf(
            line, sizeof(line), "key_%u=%d\n",
            static_cast<unsigned>(i), static_cast<int>(profile_.keys[i]));
        if (length > 0) text.append(line, static_cast<size_t>(length));
    }
    int length = sceClibSnprintf(
        line, sizeof(line),
        "left_stick_mouse=%d\nfront_touch_mouse=%d\nrear_touch_mouse=%d\n"
        "cross_mouse_click=%d\nmouse_speed=%.3f\n",
        profile_.left_stick_mouse ? 1 : 0,
        profile_.front_touch_mouse ? 1 : 0,
        profile_.rear_touch_mouse ? 1 : 0,
        profile_.cross_mouse_click ? 1 : 0,
        profile_.mouse_speed);
    if (length > 0) text.append(line, static_cast<size_t>(length));
    return vita::writeTextFile(profilePath(game_name), text);
}

bool InputMapper::loadForGame(const std::string& game_name) {
    resetDefaults();
    std::string text;
    if (!vita::readTextFile(profilePath(game_name), text)) return false;

    int profile_version = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const size_t end = text.find('\n', offset);
        const std::string line = text.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
        unsigned idx = 0;
        int value = 0;
        float fvalue = 0.0f;
        if (std::sscanf(line.c_str(), "profile_version=%d", &value) == 1) {
            profile_version = value;
        } else if (std::sscanf(line.c_str(), "key_%u=%d", &idx, &value) == 2 && idx < profile_.keys.size()) {
            profile_.keys[idx] = static_cast<FlashKey>(value);
        } else if (std::sscanf(line.c_str(), "left_stick_mouse=%d", &value) == 1) {
            profile_.left_stick_mouse = value != 0;
        } else if (std::sscanf(line.c_str(), "front_touch_mouse=%d", &value) == 1) {
            profile_.front_touch_mouse = value != 0;
        } else if (std::sscanf(line.c_str(), "rear_touch_mouse=%d", &value) == 1) {
            profile_.rear_touch_mouse = value != 0;
        } else if (std::sscanf(line.c_str(), "cross_mouse_click=%d", &value) == 1) {
            profile_.cross_mouse_click = value != 0;
        } else if (std::sscanf(line.c_str(), "mouse_speed=%f", &fvalue) == 1) {
            profile_.mouse_speed = fvalue;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }

    // Migrate the exact legacy default profile. Existing custom mappings are
    // left untouched.
    if (profile_version < 2 &&
        profile_.keys[static_cast<size_t>(VitaControl::Cross)] == FlashKey::Space &&
        profile_.keys[static_cast<size_t>(VitaControl::Circle)] == FlashKey::X &&
        profile_.keys[static_cast<size_t>(VitaControl::Square)] == FlashKey::Z &&
        profile_.keys[static_cast<size_t>(VitaControl::Triangle)] == FlashKey::C &&
        profile_.keys[static_cast<size_t>(VitaControl::L)] == FlashKey::A &&
        profile_.keys[static_cast<size_t>(VitaControl::R)] == FlashKey::S &&
        profile_.keys[static_cast<size_t>(VitaControl::Start)] == FlashKey::Enter &&
        profile_.keys[static_cast<size_t>(VitaControl::Select)] == FlashKey::Escape &&
        profile_.keys[static_cast<size_t>(VitaControl::DpadUp)] == FlashKey::Up &&
        profile_.keys[static_cast<size_t>(VitaControl::DpadDown)] == FlashKey::Down &&
        profile_.keys[static_cast<size_t>(VitaControl::DpadLeft)] == FlashKey::Left &&
        profile_.keys[static_cast<size_t>(VitaControl::DpadRight)] == FlashKey::Right) {
        profile_.keys[static_cast<size_t>(VitaControl::Cross)] = FlashKey::O;
        profile_.keys[static_cast<size_t>(VitaControl::Square)] = FlashKey::P;
        profile_.keys[static_cast<size_t>(VitaControl::Select)] = FlashKey::Backspace;
    }
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
        case FlashKey::O: return "O";
        case FlashKey::P: return "P";
        case FlashKey::Backspace: return "Backspace";
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
