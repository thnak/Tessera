// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/easing.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <vector>

namespace tessera::ui::animation {

enum class Direction {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
};

enum class FillMode {
    None,
    Forwards,
    Backwards,
    Both,
};

struct KeyframeTiming {
    double duration = 1.0;
    double delay = 0.0;
    double iterations = 1.0;
    Direction direction = Direction::Normal;
    FillMode fill = FillMode::None;
    Easing easing;
};

std::vector<style::Declaration> sample_keyframes(const style::KeyframeBlock& keyframes,
                                                 const KeyframeTiming& timing,
                                                 double elapsed_seconds);

} // namespace tessera::ui::animation
