// SPDX-License-Identifier: MIT
//
// Wayland subpixel geometry (issue #504, display-aware-rendering.md section 2): the
// `wl_output.geometry` subpixel value of the output a window is on reaches
// `WindowMetrics::pixel_geometry`.
//
// Headless and panel-free: weston's headless backend always reports `unknown`, so this test hosts a
// small fake compositor (libwayland-server) in-process that advertises wl_output globals with the
// subpixel value under test, answers the xdg-shell handshake, and sends `wl_surface.enter` /
// `leave` for the output the window is on. The real client backend connects to it over a private
// socket. Whether a real driver reports the right stripe for a real panel is a manual check.
//
// libwayland-server is not thread-safe, so the compositor runs on its own thread and the test
// thread talks to it through atomic command slots (see harness/fake_wayland_compositor.hpp).

#include "harness/fake_wayland_compositor.hpp"
#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using fake_wayland::FakeCompositor;
using tessera::hal::PixelGeometry;
using tessera::hal::WindowEvent;
using tessera::hal::WindowEventType;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "window_wayland_subpixel: " << message << "\n";
        ++failures;
    }
}

std::unique_ptr<tessera::hal::Window> open_window(const char* label) {
    tessera::hal::WindowDesc desc;
    desc.title = std::string("Tessera Wayland subpixel ") + label;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;
    return tessera::hal::create_window(desc);
}

PixelGeometry single_output(std::uint32_t wire, const char* label, int index) {
    const std::string socket =
        "tessera-subpx-" + std::to_string(getpid()) + "-" + std::to_string(index);
    FakeCompositor compositor(std::vector<std::uint32_t>{wire}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, std::string(label) + ": fake compositor socket");
        return PixelGeometry::Unknown;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window(label);
    if (window == nullptr) {
        expect(false, std::string(label) + ": create_window");
        return PixelGeometry::Unknown;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    return window->metrics().pixel_geometry;
}

void check_two_outputs() {
    const std::string socket = "tessera-subpx-" + std::to_string(getpid()) + "-pair";
    // Output 0 is horizontal RGB, output 1 is horizontal BGR; the surface enters output 1.
    FakeCompositor compositor(std::vector<std::uint32_t>{2, 3}, 1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "pair: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("pair");
    if (window == nullptr) {
        expect(false, "pair: create_window");
        return;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    expect(window->metrics().pixel_geometry == PixelGeometry::BgrHorizontal,
           "two outputs: the window takes the one it entered, not the first");

    // Moving to the other output retracts the old answer and raises a DisplayChanged.
    compositor.move_to(0);
    bool saw_display_changed = false;
    for (int attempt = 0; attempt < 200 && !saw_display_changed; ++attempt) {
        (void)window->wait_for_events(0.01);
        const std::size_t count = window->poll_events(events);
        for (std::size_t i = 0; i < count; ++i) {
            saw_display_changed |= events[i].type == WindowEventType::DisplayChanged;
        }
    }
    expect(saw_display_changed, "moving to another output raises DisplayChanged");
    expect(window->metrics().pixel_geometry == PixelGeometry::RgbHorizontal,
           "after the move the window takes the new output's layout");
}

void check_ambiguous() {
    const std::string socket = "tessera-subpx-" + std::to_string(getpid()) + "-amb";
    // Two outputs and no `enter` yet: neither is evidence, so the answer is Unknown.
    FakeCompositor compositor(std::vector<std::uint32_t>{2, 3}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "ambiguous: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("ambiguous");
    if (window == nullptr) {
        expect(false, "ambiguous: create_window");
        return;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    expect(window->metrics().pixel_geometry == PixelGeometry::Unknown,
           "two outputs and no enter is Unknown, never the first output's stripe");
}

} // namespace

int main() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    char directory[] = "/tmp/tessera-wl-XXXXXX";
    if (runtime == nullptr || runtime[0] == '\0' || access(runtime, W_OK) != 0) {
        if (mkdtemp(directory) == nullptr) {
            std::cout << "window_wayland_subpixel: skipped (no writable XDG_RUNTIME_DIR)\n";
            return 0;
        }
        setenv("XDG_RUNTIME_DIR", directory, 1);
    }
    setenv("TESSERA_WINDOW_BACKEND", "wayland", 1);

    // wl_output.subpixel: unknown 0, none 1, horizontal_rgb 2, horizontal_bgr 3, vertical_rgb 4,
    // vertical_bgr 5; 77 is a value a newer compositor might send.
    expect(single_output(2, "hrgb", 0) == PixelGeometry::RgbHorizontal, "horizontal_rgb");
    expect(single_output(3, "hbgr", 1) == PixelGeometry::BgrHorizontal, "horizontal_bgr");
    expect(single_output(1, "none", 2) == PixelGeometry::None, "none");
    expect(single_output(4, "vrgb", 3) == PixelGeometry::RgbVertical, "vertical_rgb");
    expect(single_output(0, "unknown", 4) == PixelGeometry::Unknown, "unknown");
    expect(single_output(77, "future", 5) == PixelGeometry::Unknown,
           "an unrecognised wire value is Unknown, not a guessed stripe");
    check_two_outputs();
    check_ambiguous();

    if (failures == 0) {
        std::cout << "window_wayland_subpixel: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
