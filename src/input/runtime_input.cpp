#include "runtime_input.h"

#include "../player/flash_player.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
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

constexpr size_t kBufferedInputSamples = 64;
constexpr uint64_t kPointerIdleUs = 3'000'000;

float stickDelta(uint8_t value, float speed) {
    constexpr int deadzone = 18;
    const int centered = static_cast<int>(value) - 128;
    if (std::abs(centered) <= deadzone) return 0.0f;
    return static_cast<float>(centered) / 127.0f * 10.0f * speed;
}

} // namespace

bool RuntimeInput::cursorVisible() const {
    return pointer_active_ &&
           (previous_mouse_down_ || sceKernelGetProcessTimeWide() < cursor_visible_until_us_);
}

void RuntimeInput::markPointerActivity() {
    pointer_active_ = true;
    cursor_visible_until_us_ = sceKernelGetProcessTimeWide() + kPointerIdleUs;
}

void RuntimeInput::syncMouseButton(FlashPlayer& player) {
    const bool mouse_down = cross_mouse_down_ || front_touch_down_ || rear_touch_down_;
    if (mouse_down == previous_mouse_down_) return;

    player.sendMouseMove(mouse_x_, mouse_y_);
    player.sendMouseButton(mouse_x_, mouse_y_, mouse_down);
    previous_mouse_down_ = mouse_down;
    markPointerActivity();
}

void RuntimeInput::update(FlashPlayer& player, const InputProfile& profile, uint32_t blocked_buttons) {
    if (!player.ruffleRunning()) {
        initialized_ = false;
        previous_buttons_ = 0;
        previous_mouse_down_ = false;
        cross_mouse_down_ = false;
        front_touch_down_ = false;
        rear_touch_down_ = false;
        last_pad_timestamp_ = 0;
        last_front_touch_timestamp_ = 0;
        last_rear_touch_timestamp_ = 0;
        pointer_active_ = false;
        cursor_visible_until_us_ = 0;
        return;
    }

    std::array<SceCtrlData, kBufferedInputSamples> pads{};
    int pad_count = sceCtrlPeekBufferPositive(0, pads.data(), static_cast<int>(pads.size()));
    if (pad_count < 0) pad_count = 0;
    std::sort(pads.begin(), pads.begin() + pad_count,
              [](const SceCtrlData& a, const SceCtrlData& b) { return a.timeStamp < b.timeStamp; });

    if (!initialized_) {
        if (pad_count > 0) {
            const SceCtrlData& latest = pads[pad_count - 1];
            previous_buttons_ = latest.buttons & ~blocked_buttons;
            last_pad_timestamp_ = latest.timeStamp;
            cross_mouse_down_ = profile.cross_mouse_click &&
                                (previous_buttons_ & SCE_CTRL_CROSS) != 0;
        }
        SceTouchData touch{};
        if (profile.front_touch_mouse && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0) {
            last_front_touch_timestamp_ = touch.timeStamp;
            front_touch_down_ = touch.reportNum > 0;
        }
        if (profile.rear_touch_mouse && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0) {
            last_rear_touch_timestamp_ = touch.timeStamp;
            rear_touch_down_ = touch.reportNum > 0;
        }
        previous_mouse_down_ = cross_mouse_down_ || front_touch_down_ || rear_touch_down_;
        initialized_ = true;
    }

    bool pointer_moved = false;
    float buffered_dx = 0.0f;
    float buffered_dy = 0.0f;
    for (int i = 0; i < pad_count; ++i) {
        const SceCtrlData& pad = pads[i];
        if (pad.timeStamp <= last_pad_timestamp_) continue;

        const uint32_t buttons = pad.buttons & ~blocked_buttons;
        if (profile.left_stick_mouse && !front_touch_down_ && !rear_touch_down_) {
            buffered_dx += stickDelta(pad.lx, profile.mouse_speed);
            buffered_dy += stickDelta(pad.ly, profile.mouse_speed);
        }

        for (const ButtonBinding& binding : kBindings) {
            const bool down = (buttons & binding.mask) != 0;
            const bool was_down = (previous_buttons_ & binding.mask) != 0;
            if (down != was_down) {
                const FlashKey key = profile.keys[static_cast<size_t>(binding.control)];
                if (key != FlashKey::None) player.sendKey(key, down);
            }
        }

        const bool cross_down = profile.cross_mouse_click && (buttons & SCE_CTRL_CROSS) != 0;
        if (cross_down != cross_mouse_down_) {
            if (buffered_dx != 0.0f || buffered_dy != 0.0f) {
                mouse_x_ = std::clamp(mouse_x_ + buffered_dx, 0.0f, 959.0f);
                mouse_y_ = std::clamp(mouse_y_ + buffered_dy, 0.0f, 543.0f);
                markPointerActivity();
                buffered_dx = 0.0f;
                buffered_dy = 0.0f;
            }
            cross_mouse_down_ = cross_down;
            syncMouseButton(player);
        }

        previous_buttons_ = buttons;
        last_pad_timestamp_ = pad.timeStamp;
    }

    if (buffered_dx != 0.0f || buffered_dy != 0.0f) {
        mouse_x_ = std::clamp(mouse_x_ + buffered_dx, 0.0f, 959.0f);
        mouse_y_ = std::clamp(mouse_y_ + buffered_dy, 0.0f, 543.0f);
        markPointerActivity();
        pointer_moved = true;
    }

    auto processTouchHistory = [&](SceTouchPortType port,
                                   bool enabled,
                                   uint64_t& last_timestamp,
                                   bool& source_down) {
        if (!enabled) {
            if (source_down) {
                source_down = false;
                syncMouseButton(player);
            }
            return;
        }

        std::array<SceTouchData, kBufferedInputSamples> samples{};
        int count = sceTouchPeek(port, samples.data(), static_cast<SceUInt32>(samples.size()));
        if (count < 0) count = 0;
        std::sort(samples.begin(), samples.begin() + count,
                  [](const SceTouchData& a, const SceTouchData& b) {
                      return a.timeStamp < b.timeStamp;
                  });

        bool moved = false;
        float latest_x = mouse_x_;
        float latest_y = mouse_y_;
        for (int i = 0; i < count; ++i) {
            const SceTouchData& touch = samples[i];
            if (touch.timeStamp <= last_timestamp) continue;
            const bool down = touch.reportNum > 0;
            if (down) {
                latest_x = std::clamp(touch.report[0].x * 0.5f, 0.0f, 959.0f);
                latest_y = std::clamp(touch.report[0].y * 0.5f, 0.0f, 543.0f);
                markPointerActivity();
                moved = true;
            }
            if (down != source_down) {
                if (moved) {
                    mouse_x_ = latest_x;
                    mouse_y_ = latest_y;
                    moved = false;
                }
                source_down = down;
                syncMouseButton(player);
            }
            last_timestamp = touch.timeStamp;
        }


        if (moved) {
            mouse_x_ = latest_x;
            mouse_y_ = latest_y;
            pointer_moved = true;
        }
    };

    processTouchHistory(SCE_TOUCH_PORT_FRONT, profile.front_touch_mouse,
                        last_front_touch_timestamp_, front_touch_down_);
    processTouchHistory(SCE_TOUCH_PORT_BACK, profile.rear_touch_mouse,
                        last_rear_touch_timestamp_, rear_touch_down_);

    if (pointer_moved) player.sendMouseMove(mouse_x_, mouse_y_);
    if (pointer_active_ && !previous_mouse_down_ && !cursorVisible()) {
        player.sendMouseLeave();
        pointer_active_ = false;
        cursor_visible_until_us_ = 0;
    }
}

void RuntimeInput::suspend(FlashPlayer& player, const InputProfile& profile) {
    if (player.ruffleRunning()) {
        for (const ButtonBinding& binding : kBindings) {
            if ((previous_buttons_ & binding.mask) == 0) continue;
            const FlashKey key = profile.keys[static_cast<size_t>(binding.control)];
            if (key != FlashKey::None) player.sendKey(key, false);
        }
        if (previous_mouse_down_) {
            player.sendMouseButton(mouse_x_, mouse_y_, false);
        }
        if (pointer_active_) player.sendMouseLeave();
    }
    previous_buttons_ = 0;
    previous_mouse_down_ = false;
    cross_mouse_down_ = false;
    front_touch_down_ = false;
    rear_touch_down_ = false;
    initialized_ = false;
    last_pad_timestamp_ = 0;
    last_front_touch_timestamp_ = 0;
    last_rear_touch_timestamp_ = 0;
    pointer_active_ = false;
    cursor_visible_until_us_ = 0;
}

} // namespace flashvita
