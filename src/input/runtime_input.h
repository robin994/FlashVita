#pragma once

#include "input_mapper.h"

#include <cstdint>

namespace flashvita {

class FlashPlayer;

class RuntimeInput {
public:
    void update(FlashPlayer& player, const InputProfile& profile, uint32_t blocked_buttons = 0);
    void suspend(FlashPlayer& player, const InputProfile& profile);

private:
    uint32_t previous_buttons_ = 0;
    bool previous_touch_down_ = false;
    float mouse_x_ = 480.0f;
    float mouse_y_ = 272.0f;
};

} // namespace flashvita
