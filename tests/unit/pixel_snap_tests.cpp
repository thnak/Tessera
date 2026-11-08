// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/pixel_snap.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::rendering::rhi::Rect;
using tessera::ui::layout::round_half_up;
using tessera::ui::layout::snap_rect;
using tessera::ui::layout::snap_scalar;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "pixel_snap: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool rect_is(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

} // namespace

int main() {
    if (!expect(near(round_half_up(1.49f), 1.0f), "positive down") ||
        !expect(near(round_half_up(1.5f), 2.0f), "positive half up") ||
        !expect(near(round_half_up(0.5f), 1.0f), "positive small half") ||
        !expect(near(round_half_up(-1.5f), -2.0f), "negative half away") ||
        !expect(near(round_half_up(-1.49f), -1.0f), "negative down") ||
        !expect(near(round_half_up(-0.5f), -1.0f), "negative small half")) {
        return 1;
    }

    const Rect unit = snap_rect(Rect{1.5f, 2.5f, 3.0f, 4.0f}, 1.0f);
    if (!expect(rect_is(unit, 2.0f, 3.0f, 3.0f, 4.0f), "scale 1 edges")) {
        return 2;
    }

    const Rect fractional = snap_rect(Rect{0.1f, 0.1f, 1.0f, 1.0f}, 1.25f);
    if (!expect(rect_is(fractional, 0.0f, 0.0f, 0.8f, 0.8f), "scale 1.25 edges")) {
        return 3;
    }

    if (!expect(near(snap_scalar(0.25f, 2.0f), 0.5f), "scale 2 half") ||
        !expect(near(snap_scalar(0.24f, 2.0f), 0.0f), "scale 2 down") ||
        !expect(near(snap_scalar(0.75f, 2.0f), 1.0f), "scale 2 up")) {
        return 4;
    }

    const Rect clamped = snap_rect(Rect{0.1f, 0.1f, 1.0f, 1.0f}, 0.5f);
    if (!expect(rect_is(clamped, 0.0f, 0.0f, 1.0f, 1.0f), "scale below one clamps")) {
        return 5;
    }

    const Rect left = snap_rect(Rect{0.0f, 0.0f, 1.1f, 1.0f}, 1.25f);
    const Rect right = snap_rect(Rect{1.1f, 0.0f, 0.3f, 1.0f}, 1.25f);
    if (!expect(near(left.x + left.width, right.x), "adjacent edges stay joined") ||
        !expect(near(left.width, 0.8f), "left width") ||
        !expect(near(right.width, 0.8f), "right width")) {
        return 6;
    }

    const Rect input{0.1f, 0.1f, 1.0f, 1.0f};
    const Rect untouched = input;
    const Rect snapped = snap_rect(input, 1.25f);
    (void)snapped;
    if (!expect(rect_is(input, untouched.x, untouched.y, untouched.width, untouched.height),
                "input remains fractional")) {
        return 7;
    }

    std::cout << "pixel_snap: ok\n";
    return 0;
}
