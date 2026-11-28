// SPDX-License-Identifier: MIT
//
// IME spot anchoring (issue #55): logical caret rect -> XNSpotLocation pixels, clamping, rejection
// of unusable input and change tracking. Pure, no X server.

#include "x11_ime_spot.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using tessera::hal::TextInputRect;
using namespace tessera::hal::x11;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "x11_ime_spot: " << message << "\n";
    }
    return condition;
}

bool mapping() {
    SpotLocation spot{};
    bool ok = expect(spot_from_rect({10.0F, 20.0F, 2.0F, 16.0F}, 1.0F, 800, 600, spot) &&
                         spot.x == 10 && spot.y == 36,
                     "spot is the bottom-left of the caret rect");
    ok = expect(spot_from_rect({10.0F, 20.0F, 2.0F, 16.0F}, 2.0F, 1600, 1200, spot) &&
                    spot.x == 20 && spot.y == 72,
                "logical pixels scale to physical") &&
         ok;
    ok = expect(spot_from_rect({-50.0F, -5.0F, 2.0F, 10.0F}, 1.0F, 800, 600, spot) && spot.x == 0 &&
                    spot.y == 5,
                "negative x clamps to the window edge") &&
         ok;
    ok = expect(spot_from_rect({5000.0F, 9000.0F, 2.0F, 10.0F}, 1.0F, 800, 600, spot) &&
                    spot.x == 799 && spot.y == 599,
                "a caret past the window clamps inside it") &&
         ok;
    return ok;
}

bool rejection() {
    SpotLocation spot{7, 9};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bool ok =
        expect(!spot_from_rect({nan, 0.0F, 1.0F, 1.0F}, 1.0F, 800, 600, spot), "NaN rejected");
    ok = expect(!spot_from_rect({0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, 800, 600, spot),
                "zero scale rejected") &&
         ok;
    ok = expect(!spot_from_rect({0.0F, 0.0F, 1.0F, 1.0F}, 1.0F, 0, 600, spot),
                "empty window rejected") &&
         ok;
    return expect(spot.x == 7 && spot.y == 9, "a rejected rect leaves the output alone") && ok;
}

bool tracking() {
    SpotTracker tracker;
    SpotLocation spot{};
    const TextInputRect rect{10.0F, 20.0F, 2.0F, 16.0F};
    bool ok = expect(tracker.update(rect, 1.0F, 800, 600, spot), "first rect is sent");
    ok = expect(!tracker.update(rect, 1.0F, 800, 600, spot), "same rect is not resent") && ok;
    ok = expect(tracker.update({10.0F, 40.0F, 2.0F, 16.0F}, 1.0F, 800, 600, spot) && spot.y == 56,
                "a moved caret is sent") &&
         ok;
    tracker.reset();
    ok = expect(tracker.update({10.0F, 40.0F, 2.0F, 16.0F}, 1.0F, 800, 600, spot),
                "reset forces a resend (new input context)") &&
         ok;
    return ok;
}

} // namespace

int main() {
    return (mapping() && rejection() && tracking()) ? 0 : 1;
}
