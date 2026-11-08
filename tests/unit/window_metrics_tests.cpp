// SPDX-License-Identifier: MIT

#include "tessera/hal/window.hpp"

#include <cmath>
#include <iostream>
#include <iterator>
#include <type_traits>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_metrics: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-6f) {
    return std::abs(value - expected) <= tolerance;
}

} // namespace

int main() {
    using tessera::hal::Window;
    using tessera::hal::WindowEvent;
    using tessera::hal::WindowEventType;
    using tessera::hal::WindowMetrics;

    static_assert(std::is_abstract_v<Window>, "Window must be a pure interface");
    static_assert(std::is_default_constructible_v<WindowMetrics>,
                  "WindowMetrics must be default-constructible");

    const WindowMetrics defaults;
    if (!expect(defaults.logical_width == 0 && defaults.logical_height == 0,
                "default logical size is zero") ||
        !expect(defaults.physical_width == 0 && defaults.physical_height == 0,
                "default physical size is zero") ||
        !expect(near(defaults.dpi_scale, 1.0f), "default dpi scale is 1") ||
        !expect(near(defaults.insets.left, 0.0f) && near(defaults.insets.top, 0.0f) &&
                    near(defaults.insets.right, 0.0f) && near(defaults.insets.bottom, 0.0f),
                "default insets are zero") ||
        !expect(defaults.focused, "default focused") ||
        !expect(!defaults.occluded, "default not occluded") ||
        !expect(!defaults.minimized, "default not minimized")) {
        return 1;
    }

    WindowMetrics metrics;
    metrics.logical_width = 1280;
    metrics.logical_height = 720;
    metrics.physical_width = 2560;
    metrics.physical_height = 1440;
    metrics.dpi_scale = 2.0f;
    metrics.insets.left = 8.0f;
    metrics.insets.top = 16.0f;
    metrics.insets.right = 8.0f;
    metrics.insets.bottom = 24.0f;
    metrics.focused = false;
    metrics.occluded = true;
    metrics.minimized = false;

    if (!expect(metrics.logical_width == 1280 && metrics.logical_height == 720,
                "logical size fields") ||
        !expect(metrics.physical_width == 2560 && metrics.physical_height == 1440,
                "physical size fields") ||
        !expect(near(metrics.dpi_scale, 2.0f), "dpi scale field") ||
        !expect(near(metrics.insets.left, 8.0f) && near(metrics.insets.top, 16.0f) &&
                    near(metrics.insets.right, 8.0f) && near(metrics.insets.bottom, 24.0f),
                "inset fields") ||
        !expect(!metrics.focused && metrics.occluded && !metrics.minimized, "lifecycle fields")) {
        return 2;
    }

    WindowEvent event;
    if (!expect(event.type == WindowEventType::Resize, "default event type") ||
        !expect(event.metrics.logical_width == 0, "default event metrics")) {
        return 3;
    }

    event.type = WindowEventType::DpiChange;
    event.metrics = metrics;
    if (!expect(event.type == WindowEventType::DpiChange, "event type is assignable") ||
        !expect(event.metrics.logical_width == 1280 && near(event.metrics.dpi_scale, 2.0f),
                "event carries metrics")) {
        return 4;
    }

    // The normalized taxonomy is distinct and ordered as documented.
    constexpr WindowEventType kAllTypes[] = {
        WindowEventType::Resize,   WindowEventType::DpiChange, WindowEventType::Focus,
        WindowEventType::Blur,     WindowEventType::Occluded,  WindowEventType::Minimized,
        WindowEventType::Restored, WindowEventType::Close,
    };
    for (std::size_t i = 0; i < std::size(kAllTypes); ++i) {
        for (std::size_t j = i + 1; j < std::size(kAllTypes); ++j) {
            if (!expect(static_cast<int>(kAllTypes[i]) != static_cast<int>(kAllTypes[j]),
                        "event types are distinct")) {
                return 5;
            }
        }
    }

    std::cout << "window_metrics: ok\n";
    return 0;
}
