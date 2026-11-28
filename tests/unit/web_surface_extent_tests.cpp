// SPDX-License-Identifier: MIT

// Issue #1013: the canvas backing-store size rule shared by the surface and the host glue.

#include "web_surface_extent.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using namespace tessera::hal::web;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "web_surface_extent: " << message << "\n";
        ++g_failures;
    }
}

} // namespace

int main() {
    expect(backing_extent({800.0F, 600.0F, 1.0F}) == WebSurfaceExtent{800, 600},
           "ratio 1 keeps the css size");
    expect(backing_extent({800.0F, 600.0F, 2.0F}) == WebSurfaceExtent{1600, 1200},
           "ratio 2 doubles it");
    expect(backing_extent({333.0F, 101.0F, 1.5F}) == WebSurfaceExtent{500, 152},
           "fractions round to nearest");
    expect(backing_extent({0.0F, 0.0F, 2.0F}) == WebSurfaceExtent{1, 1}, "a zero canvas is 1x1");
    expect(backing_extent({-5.0F, 10.0F, 1.0F}) == WebSurfaceExtent{1, 10}, "a negative axis is 1");
    expect(backing_extent({0.2F, 0.2F, 1.0F}) == WebSurfaceExtent{1, 1},
           "sub-pixel sizes floor to 1");
    expect(backing_extent({100.0F, 100.0F, 0.0F}) == WebSurfaceExtent{100, 100},
           "ratio 0 behaves as 1");
    expect(backing_extent({100.0F, 100.0F, std::nanf("")}) == WebSurfaceExtent{100, 100},
           "NaN ratio behaves as 1");
    const float inf = std::numeric_limits<float>::infinity();
    expect(backing_extent({inf, 50.0F, 1.0F}) == WebSurfaceExtent{1, 50}, "an infinite size is 1");
    expect(backing_extent({5000.0F, 5000.0F, 4.0F}) == WebSurfaceExtent{8192, 8192},
           "clamped to the texture limit");
    expect(backing_extent({100.0F, 100.0F, 3.0F}, 200) == WebSurfaceExtent{200, 200},
           "a custom limit applies");
    expect(backing_extent({1.0e30F, 1.0F, 1.0F}) == WebSurfaceExtent{8192, 1},
           "huge sizes do not overflow");

    if (g_failures == 0) {
        std::cout << "web_surface_extent: ok\n";
    }
    return g_failures == 0 ? 0 : 1;
}
