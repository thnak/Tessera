// SPDX-License-Identifier: MIT
//
// Wayland monitor list (issue #506): `Window::displays()` is built from the bound wl_outputs, and a
// change to the list (mode, scale, hotplug) raises `DisplayChanged`.
//
// Headless and panel-free: the in-process fake compositor (harness/fake_wayland_compositor.hpp)
// advertises wl_output globals with the properties under test and re-sends or removes them on
// command. Whether a real compositor reports the same for a real panel is a manual check.

#include "harness/fake_wayland_compositor.hpp"
#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using fake_wayland::FakeCompositor;
using fake_wayland::OutputSpec;
using tessera::hal::DisplayInfo;
using tessera::hal::PixelGeometry;
using tessera::hal::WindowEvent;
using tessera::hal::WindowEventType;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "window_wayland_displays: " << message << "\n";
        ++failures;
    }
}

std::string socket_name(const char* label) {
    return std::string("tessera-displays-") + std::to_string(getpid()) + "-" + label;
}

std::unique_ptr<tessera::hal::Window> open_window(const char* label) {
    tessera::hal::WindowDesc desc;
    desc.title = std::string("Tessera Wayland displays ") + label;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;
    auto window = tessera::hal::create_window(desc);
    if (window != nullptr) {
        std::array<WindowEvent, 32> events{};
        (void)window->poll_events(events);
    }
    return window;
}

std::size_t list_displays(const tessera::hal::Window& window, std::array<DisplayInfo, 8>& out) {
    out = {};
    return window.displays(out);
}

// Polls until a DisplayChanged arrives or the attempts run out.
bool wait_for_display_changed(tessera::hal::Window& window) {
    std::array<WindowEvent, 32> events{};
    for (int attempt = 0; attempt < 300; ++attempt) {
        (void)window.wait_for_events(0.01);
        const std::size_t count = window.poll_events(events);
        for (std::size_t i = 0; i < count; ++i) {
            if (events[i].type == WindowEventType::DisplayChanged) {
                return true;
            }
        }
    }
    return false;
}

void check_single_output() {
    OutputSpec spec;
    spec.subpixel = 2; // horizontal_rgb
    spec.width = 3840;
    spec.height = 2160;
    spec.refresh_mhz = 59997;
    spec.scale = 2;
    const std::string socket = socket_name("one");
    FakeCompositor compositor({spec}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "single: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("one");
    if (window == nullptr) {
        expect(false, "single: create_window");
        return;
    }
    std::array<DisplayInfo, 8> displays{};
    expect(list_displays(*window, displays) == 1, "single: one output is listed");
    const DisplayInfo& info = displays[0];
    expect(info.id != tessera::hal::kNoDisplayId, "single: the output has an id");
    expect(info.physical_width == 3840 && info.physical_height == 2160,
           "single: physical size is the current mode");
    expect(info.scale == 2.0f && !info.scale_assumed, "single: integer scale is measured");
    expect(info.bounds.width == 1920.0f && info.bounds.height == 1080.0f,
           "single: bounds are logical pixels (mode / scale)");
    expect(!info.refresh_hz_assumed && std::fabs(info.refresh_hz - 59.997f) < 0.001f,
           "single: refresh comes from the mode in millihertz");
    expect(info.pixel_geometry == PixelGeometry::RgbHorizontal,
           "single: DisplayInfo carries the subpixel layout");
    expect(std::string(info.name.data()) == "Tessera Fake", "single: name is make and model");
}

void check_rotated_output() {
    OutputSpec spec;
    spec.transform = 1; // 90 degrees: the mode's axes swap in logical space
    const std::string socket = socket_name("rot");
    FakeCompositor compositor({spec}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "rotated: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("rot");
    if (window == nullptr) {
        expect(false, "rotated: create_window");
        return;
    }
    std::array<DisplayInfo, 8> displays{};
    expect(list_displays(*window, displays) == 1, "rotated: one output is listed");
    expect(displays[0].physical_width == 1080 && displays[0].physical_height == 1920,
           "rotated: a 90 degree output swaps width and height");
}

void check_two_outputs_and_changes() {
    OutputSpec left;
    left.subpixel = 2;
    OutputSpec right;
    right.subpixel = 3;
    right.x = 1920;
    const std::string socket = socket_name("pair");
    FakeCompositor compositor({left, right}, -1, socket.c_str());
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
    std::array<DisplayInfo, 8> displays{};
    expect(list_displays(*window, displays) == 2, "pair: both outputs are listed");
    expect(displays[0].bounds.x == 0.0f && displays[1].bounds.x == 1920.0f,
           "pair: each output keeps its place in the compositor space");
    expect(displays[0].id != displays[1].id, "pair: outputs have distinct ids");
    expect(displays[0].pixel_geometry == PixelGeometry::RgbHorizontal &&
               displays[1].pixel_geometry == PixelGeometry::BgrHorizontal,
           "pair: each output carries its own subpixel layout");

    std::array<DisplayInfo, 1> short_list{};
    expect(window->displays(short_list) == 1, "pair: the list is bounded by the caller's span");

    // A mode change on output 1 re-sends its batch ending in `done`.
    OutputSpec changed = right;
    changed.width = 2560;
    changed.height = 1440;
    compositor.reconfigure(1, changed);
    expect(wait_for_display_changed(*window), "a mode change raises DisplayChanged");
    expect(list_displays(*window, displays) == 2 && displays[1].physical_width == 2560 &&
               displays[1].physical_height == 1440,
           "the list reflects the new mode");

    // Unplugging output 0 shrinks the list.
    compositor.remove_output(0);
    expect(wait_for_display_changed(*window), "removing an output raises DisplayChanged");
    expect(list_displays(*window, displays) == 1 && displays[0].bounds.x == 1920.0f,
           "the removed output leaves the list");
}

} // namespace

int main() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    char directory[] = "/tmp/tessera-wl-XXXXXX";
    if (runtime == nullptr || runtime[0] == '\0' || access(runtime, W_OK) != 0) {
        if (mkdtemp(directory) == nullptr) {
            std::cout << "window_wayland_displays: skipped (no writable XDG_RUNTIME_DIR)\n";
            return 0;
        }
        setenv("XDG_RUNTIME_DIR", directory, 1);
    }
    setenv("TESSERA_WINDOW_BACKEND", "wayland", 1);

    check_single_output();
    check_rotated_output();
    check_two_outputs_and_changes();

    if (failures == 0) {
        std::cout << "window_wayland_displays: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
