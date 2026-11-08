// SPDX-License-Identifier: MIT

#include "tessera/rendering/damage.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-6f) {
    return std::abs(value - expected) <= tolerance;
}

bool same_rect(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

} // namespace

int main() {
    DamageAccumulator damage(DamageConfig{Extent2D{100, 100}, 4});
    if (!expect(damage.empty(), "new accumulator is empty")) {
        return 1;
    }

    damage.add(Rect{10, 10, 20, 20});
    if (!expect(damage.rects().size() == 1, "first rect") ||
        !expect(same_rect(damage.rects()[0], 10, 10, 20, 20), "first bounds")) {
        return 2;
    }

    damage.add(Rect{25, 25, 20, 20});
    if (!expect(damage.rects().size() == 1, "overlap merges") ||
        !expect(same_rect(damage.rects()[0], 10, 10, 35, 35), "merged bounds")) {
        return 3;
    }

    damage.add(Rect{80, 80, 10, 10});
    if (!expect(damage.rects().size() == 2, "disjoint appends")) {
        return 4;
    }

    damage.add(Rect{-20, -20, 10, 10});
    if (!expect(damage.rects().size() == 2, "outside rect ignored")) {
        return 5;
    }

    damage.add(Rect{90, 90, 50, 50});
    if (!expect(damage.rects().size() == 2, "touch merges") ||
        !expect(same_rect(damage.rects()[1], 80, 80, 20, 20), "clipped touch bounds")) {
        return 6;
    }

    damage.add(Rect{60, 60, 1, 1}, 2.0f);
    if (!expect(damage.rects().size() == 3, "margin adds separate rect")) {
        return 7;
    }

    damage.clear();
    if (!expect(damage.empty(), "clear empties") || !expect(!damage.full_redraw(), "clear full")) {
        return 8;
    }

    DamageAccumulator capped(DamageConfig{Extent2D{100, 100}, 1});
    capped.add(Rect{1, 1, 2, 2});
    capped.add(Rect{50, 50, 2, 2});
    if (!expect(capped.full_redraw(), "cap falls back to full redraw") ||
        !expect(capped.rects().empty(), "full redraw clears rects")) {
        return 9;
    }

    capped.clear();
    if (!expect(!capped.full_redraw(), "full redraw cleared")) {
        return 10;
    }

    bool threw = false;
    try {
        DamageAccumulator invalid(DamageConfig{Extent2D{10, 10}, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero max_rects rejected")) {
        return 11;
    }

    threw = false;
    try {
        capped.add(Rect{1, 1, 2, 2}, -1.0f);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "negative margin rejected")) {
        return 12;
    }

    std::cout << "damage: ok\n";
    return 0;
}
