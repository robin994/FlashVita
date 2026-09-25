#include "runtime_input.h"

#include "../player/flash_player.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace flashvita {
namespace {

struct ButtonBinding {
    uint32_t mask;
    VitaControl control;
};

constexpr std::array<ButtonBinding, static_cast<size_t>(VitaControl::Count)> kBindings{{
    {SCE_CTRL_CROSS, VitaControl::Cross},
    {SCE_CTRL_CIRCLE, VitaControl::Circle},
    {SCE_CTRL_SQUARE, VitaControl::Square},
    {SCE_CTRL_TRIANGLE, VitaControl::Triangle},
    {SCE_CTRL_LTRIGGER, VitaControl::L},
    {SCE_CTRL_RTRIGGER, VitaControl::R},
    {SCE_CTRL_START, VitaControl::Start},
    {SCE_CTRL_SELECT, VitaControl::Select},
    {SCE_CTRL_UP, VitaControl::DpadUp},
    {SCE_CTRL_DOWN, VitaControl::DpadDown},
    {SCE_CTRL_LEFT, VitaControl::DpadLeft},
    {SCE_CTRL_RIGHT, VitaControl::DpadRight},
}};

float stickDelta(uint8_t value, float speed) {
    constexpr int deadzone = 18;
    const int centered = static_cast<int>(value) - 128;
    if (std::abs(centered) <= deadzone) return 0.0f;
    return static_cast<float>(centered) / 127.0f * 10.0f * speed;
}

} // namespace

void RuntimeInput::update(FlashPlayer& player, const InputProfile& profile, uint32_t blocked_buttons) {
    if (!player.ruffleRunning()) {
        previous_buttons_ = 0;
        previous_touch_down_ = false;
        return;
    }

    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    pad.buttons &= ~blocked_buttons;

    for (const ButtonBinding& binding : kBindings) {
        const bool down = (pad.buttons & binding.mask) != 0;
        const bool was_down = (previous_buttons_ & binding.mask) != 0;
        if (down != was_down) {
            const FlashKey key = profile.keys[static_cast<size_t>(binding.control)];
            if (key != FlashKey::None) player.sendKey(key, down);
        }
    }
    previous_buttons_ = pad.buttons;

    bool touch_down = false;
    if (profile.front_touch_mouse) {
        SceTouchData touch{};
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0) {
            mouse_x_ = std::clamp(touch.report[0].x * 0.5f, 0.0f, 959.0f);
            mouse_y_ = std::clamp(touch.report[0].y * 0.5f, 0.0f, 543.0f);
            touch_down = true;
            player.sendMouseMove(mouse_x_, mouse_y_);
        }
    }

    if (!touch_down && profile.rear_touch_mouse) {
        SceTouchData touch{};
        if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0 && touch.reportNum > 0) {
            mouse_x_ = std::clamp(touch.report[0].x * 0.5f, 0.0f, 959.0f);
            mouse_y_ = std::clamp(touch.report[0].y * 0.5f, 0.0f, 543.0f);
            touch_down = true;
            player.sendMouseMove(mouse_x_, mouse_y_);
        }
    }

    if (!touch_down && profile.left_stick_mouse) {
        const float dx = stickDelta(pad.lx, profile.mouse_speed);
        const float dy = stickDelta(pad.ly, profile.mouse_speed);
        if (dx != 0.0f || dy != 0.0f) {
            mouse_x_ = std::clamp(mouse_x_ + dx, 0.0f, 959.0f);
            mouse_y_ = std::clamp(mouse_y_ + dy, 0.0f, 543.0f);
            player.sendMouseMove(mouse_x_, mouse_y_);
        }
    }

    if (touch_down != previous_touch_down_) {
        player.sendMouseButton(mouse_x_, mouse_y_, touch_down);
        previous_touch_down_ = touch_down;
    }
}

void RuntimeInput::suspend(FlashPlayer& player, const InputProfile& profile) {
    if (player.ruffleRunning()) {
        for (const ButtonBinding& binding : kBindings) {
            if ((previous_buttons_ & binding.mask) == 0) continue;
            const FlashKey key = profile.keys[static_cast<size_t>(binding.control)];
            if (key != FlashKey::None) player.sendKey(key, false);
        }
        if (previous_touch_down_) {
            player.sendMouseButton(mouse_x_, mouse_y_, false);
        }
    }
    previous_buttons_ = 0;
    previous_touch_down_ = false;
}

} // namespace flashvita
