// SPDX-License-Identifier: MIT
//
// Wayland window smoke test (issue #155): open a window, observe the first compositor configure,
// present a frame (request a redraw / poll), close it, and see the Close event delivered.
//
// It skips with a diagnostic on a box without a Wayland compositor - that is the documented
// behaviour of the acceptance criterion ("skipped, with a diagnostic, where no compositor exists"),
// so the same ctest entry is meaningful on a Wayland desktop, under a headless weston, and in CI
// where the test simply skips.
//
// The presentation half (Vulkan swapchain + one presented frame on the Wayland surface) is
// tests/gpu/vulkan/vulkan_swapchain_wayland_tests.cpp; this file is the window-only gate and needs
// no GPU.

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_wayland: " << message << "\n";
    }
    return condition;
}

bool contains_event(const std::array<tessera::hal::WindowEvent, 32>& events, std::size_t count,
                    tessera::hal::WindowEventType type) {
    for (std::size_t i = 0; i < count; ++i) {
        if (events[i].type == type) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using tessera::hal::WindowEvent;
    using tessera::hal::WindowEventType;
    using tessera::hal::WindowMetrics;

    // A Wayland session is named by WAYLAND_DISPLAY. Without it the compositor cannot be reached at
    // all, which is the "no compositor" skip, not a failure.
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display == nullptr || wayland_display[0] == '\0') {
        std::cout
            << "window_wayland: skipped (no Wayland compositor: WAYLAND_DISPLAY is not set)\n";
        return 0;
    }
    // Pin the backend: on a Wayland desktop the selector would pick Wayland anyway, and under
    // XWayland-only CI there is no Wayland to pick. Pinning makes the failure mode a clear skip
    // instead of silently testing the X11 backend.
    setenv("TESSERA_WINDOW_BACKEND", "wayland", 1);

    tessera::hal::WindowDesc desc;
    desc.title =
        "Tessera Wayland window test " + std::to_string(static_cast<unsigned long>(getpid()));
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;

    auto window = tessera::hal::create_window(desc);
    if (window == nullptr) {
        // create_window() already printed TESSERA_WINDOW_WAYLAND_UNSUPPORTED with the reason.
        std::cout << "window_wayland: skipped (no Wayland compositor: connection failed)\n";
        return 0;
    }
    if (!expect(window->window_system() == tessera::hal::WindowSystem::Wayland,
                "the backend reports the Wayland window system")) {
        return 1;
    }

    // The compositor owns the size: the metrics must report a non-zero logical size after the
    // initial configure sequence, and the surface/display pair the renderer presents through must
    // be non-null.
    const WindowMetrics initial = window->metrics();
    if (!expect(initial.logical_width > 0 && initial.logical_height > 0,
                "the first configure yields a non-zero logical size") ||
        !expect(initial.physical_width == initial.logical_width &&
                    initial.physical_height == initial.logical_height,
                "logical and physical size agree at integer scale 1.0") ||
        !expect(initial.dpi_scale == 1.0f, "the documented scale-1.0 answer is reported") ||
        !expect(!initial.focused && !initial.occluded && !initial.minimized,
                "a freshly created window reports no lifecycle state") ||
        !expect(!initial.fullscreen,
                "a toplevel is not full-screen unless the compositor says so")) {
        return 2;
    }
    const tessera::hal::NativeWindowHandle native = window->native_handle();
    if (!expect(native.display != nullptr && native.window != nullptr,
                "the wl_display/wl_surface pair is published for the renderer")) {
        return 3;
    }
    // A Wayland backend has no display-refresh source this slice (no wl_output/frame callbacks), so
    // the documented answer is "unknown", never a guess.
    if (!expect(window->display_refresh_hz() == tessera::hal::kUnknownRefreshHz,
                "no wl_output is bound, so the refresh rate is unknown")) {
        return 4;
    }

    // Creation-time configure traffic is state, not events.
    std::array<WindowEvent, 32> events{};
    std::size_t count = 0;
    if (!expect(window->poll_events(events) == 0, "creation records no event")) {
        return 5;
    }

    // Open: the surface becomes logically visible and the redraw request is honoured through the
    // wait/poll wake. Wayland has no invalidation request, so the HAL's normalized redraw flag is
    // what wakes the loop; the frame itself is presented by the renderer.
    window->show();
    window->request_redraw();
    const double start = tessera::hal::steady_now_seconds();
    const bool woke = window->wait_for_events(1.0);
    const double elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(woke, "request_redraw wakes the event wait") ||
        !expect(elapsed < 2.0, "the wake is bounded")) {
        return 6;
    }
    // One bounded poll drains whatever the compositor sent for the map (a configure, an activated
    // state) without ever blocking. A compositor is free to map without sending a second configure,
    // so no event is a valid outcome here; what must hold is that the poll never blocks and the
    // window keeps a consistent size.
    count = window->poll_events(events);
    if (!expect(window->metrics().logical_width > 0,
                "the window keeps a valid size after mapping")) {
        return 7;
    }
    (void)count;

    // An idle window times out instead of spinning, and a negative timeout degrades to a poll.
    const double idle_start = tessera::hal::steady_now_seconds();
    const bool has_event = window->wait_for_events(0.03);
    const double idle_elapsed = tessera::hal::steady_now_seconds() - idle_start;
    if (!expect(!has_event, "an idle wait reports no event") ||
        !expect(idle_elapsed >= 0.02, "the idle wait parks for the timeout") ||
        !expect(idle_elapsed < 2.0, "the idle wait is bounded")) {
        return 8;
    }
    if (!expect(!window->wait_for_events(-1.0), "a negative timeout reports no event") ||
        !expect(tessera::hal::steady_now_seconds() - idle_start < 2.0,
                "a negative timeout returns immediately")) {
        return 9;
    }

    // Close: Window::close() enqueues the normalized Close event, and the next poll delivers it
    // exactly once. A second close is a no-op (the same contract the X11 backend has).
    window->close();
    count = window->poll_events(events);
    if (!expect(count > 0 && contains_event(events, count, WindowEventType::Close),
                "close() produces a Close event")) {
        return 10;
    }
    window->close();
    if (!expect(window->poll_events(events) == 0, "a second close is a no-op")) {
        return 11;
    }

    const WindowMetrics final_metrics = window->metrics();
    window.reset();

    // An invalid descriptor is rejected before any compositor object exists.
    tessera::hal::WindowDesc invalid = desc;
    invalid.width = 0;
    if (!expect(tessera::hal::create_window(invalid) == nullptr, "zero width is rejected")) {
        return 12;
    }

    std::cout << "window_wayland: ok (" << initial.logical_width << "x" << initial.logical_height
              << " on " << (wayland_display != nullptr ? wayland_display : "?") << ", final "
              << final_metrics.logical_width << "x" << final_metrics.logical_height << ")\n";
    return 0;
}
