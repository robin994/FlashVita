#pragma once

#include "input_mapper.h"

#include <cstdint>

namespace flashvita {

class FlashPlayer;

class RuntimeInput {
public:
    void update(FlashPlayer& player, const InputProfile& profile, uint32_t blocked_buttons = 0);
    void suspend(FlashPlayer& player, const InputProfile& profile);
    bool cursorVisible() const;
    float mouseX() const { return mouse_x_; }
    float mouseY() const { return mouse_y_; }

private:
    void syncMouseButton(FlashPlayer& player);
    void markPointerActivity();

    bool initialized_ = false;
    uint32_t previous_buttons_ = 0;
    bool previous_mouse_down_ = false;
    bool cross_mouse_down_ = false;
    bool front_touch_down_ = false;
    bool rear_touch_down_ = false;
    uint64_t last_pad_timestamp_ = 0;
    uint64_t last_front_touch_timestamp_ = 0;
    uint64_t last_rear_touch_timestamp_ = 0;
    float mouse_x_ = 480.0f;
    float mouse_y_ = 272.0f;
    bool pointer_active_ = false;
    uint64_t cursor_visible_until_us_ = 0;
};

} // namespace flashvita
