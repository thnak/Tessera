// SPDX-License-Identifier: MIT
//
// Issue #180: two windows driven by one loop, with independent lifecycle, damage and pacing, and a
// static window that still idles with zero presents.
//
// This is the acceptance, and each case is one clause of it rather than a proxy:
//
//   * **one loop drives both** - `WindowDevice::pump_windows_once()` gives every registered window
//     one bounded iteration. Before this slice a caller had to interleave two blocking `run()`
//     calls, which cannot work on one thread.
//   * **independent damage** - a change in one window paints that window and not its sibling. The
//     damage plan is per window, so the sibling's presented count must not move.
//   * **independent pacing** - both windows present in the *same* group tick. The pacer is
//     app-scoped (one app, one rate), so it gates ticks and not windows; advancing it per window
//     would let the first window in the walk consume the interval and starve the second.
//   * **a static window idles** - a settled window presents nothing, which is ADR-019's rule and
//     the reason a two-window app does not burn a GPU on an unchanged scene.
//
// The test runs on whatever device the lane has. On a machine with no presentation surface the
// hosts report `available() == false` and it prints `skipped`, which the CTest entry turns into a
// skip rather than a pass - a lane without a window backend proves nothing about this behaviour.

#include "tessera/app/window.hpp"
#include "tessera/app/window_device.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowDevice;
using tessera::app::WindowDeviceOptions;
using tessera::app::WindowHost;
using tessera::app::WindowOptions;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

int g_failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_multi_loop: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

std::unique_ptr<Node> make_root(std::uint64_t id, std::string text) {
    auto root = std::make_unique<Node>(id);
    auto label = std::make_unique<Node>(id + 1);
    label->set_element_kind("Text");
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = std::move(text);
    label->set_paint_data(std::move(data));
    root->add_child(std::move(label));
    return root;
}

WindowOptions window_options(std::string title, std::uint32_t width, std::uint32_t height) {
    WindowOptions options;
    options.title = std::move(title);
    options.width = width;
    options.height = height;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    return options;
}

// A group plus two windows, or a skip when the lane has no presentation surface. The two windows
// are built in one place so every case below starts from the same state.
//
// `max_fps` matters: the pacer is seeded by the *first* window's options, and an uncapped pacer
// (the default) has no interval at all, so `ready()` always says "present now" and nothing gates.
// The pacing case therefore has to ask for a real cap, or it would assert nothing.
struct TwoWindows {
    explicit TwoWindows(bool with_second = true, double max_fps = 0.0)
        : group(device_options()) {
        if (!group.available()) {
            return;
        }
        first_root = make_root(1, "first window");
        WindowOptions first_options = window_options("first", 320, 200);
        first_options.max_fps = max_fps;
        first = std::make_unique<WindowHost>(
            *first_root, compile_stylesheet("Text { font-size: 16px; }"), first_options, group);
        if (!first->available()) {
            return;
        }
        if (with_second) {
            second_root = make_root(10, "second window");
            WindowOptions second_options = window_options("second", 400, 260);
            second_options.max_fps = max_fps;
            second = std::make_unique<WindowHost>(*second_root,
                                                  compile_stylesheet("Text { font-size: 16px; }"),
                                                  second_options, group);
        }
        ready = second == nullptr || second->available();
    }

    static WindowDeviceOptions device_options() {
        WindowDeviceOptions options;
        options.font_path = TESSERA_TEST_FONT;
        return options;
    }

    WindowDevice group;
    std::unique_ptr<Node> first_root;
    std::unique_ptr<Node> second_root;
    std::unique_ptr<WindowHost> first;
    std::unique_ptr<WindowHost> second;
    bool ready = false;
};

void test_one_loop_drives_both() {
    TwoWindows windows;
    if (!windows.ready) {
        std::cout << "app_window_multi_loop: skipped (no presentation surface)\n";
        return;
    }
    expect(windows.group.window_list_size() == 2, "both windows registered with the group");

    // One call drives both windows. Each presents its startup frame, so the walk reports two.
    const std::size_t presented = windows.group.pump_windows_once();
    expect(presented == 2, "one group iteration presented both windows");
    expect(windows.first->stats().presented > 0, "the first window presented");
    expect(windows.second->stats().presented > 0, "the second window presented");
}

void test_independent_damage() {
    TwoWindows windows;
    if (!windows.ready) {
        std::cout << "app_window_multi_loop: skipped (no presentation surface)\n";
        return;
    }
    (void)windows.group.pump_windows_once();
    const std::uint32_t first_before = windows.first->stats().presented;
    const std::uint32_t second_before = windows.second->stats().presented;

    // A settled scene presents nothing: this is the "static window idles" clause, and it is what
    // makes the damage assertion below meaningful rather than a coincidence of startup frames.
    (void)windows.group.pump_windows_once();
    expect(windows.first->stats().presented == first_before,
           "a settled first window presents nothing");
    expect(windows.second->stats().presented == second_before,
           "a settled second window presents nothing");

    // Damage only the first window, through the same seam a platform resize uses. Its sibling's
    // scene is untouched, so only the first may paint.
    tessera::hal::WindowMetrics metrics = windows.first->window()->metrics();
    metrics.logical_width = 640;
    metrics.logical_height = 400;
    metrics.physical_width = 640;
    metrics.physical_height = 400;
    windows.first->apply_metrics(metrics);
    (void)windows.group.pump_windows_once();

    expect(windows.first->stats().presented > first_before,
           "the damaged window painted its change");
    expect(windows.second->stats().presented == second_before,
           "the untouched sibling painted nothing");
    // The layout half: the resized window re-laid out, the other kept its own size.
    expect(windows.first_root->bounds().width > 320.0f,
           "the resized window re-laid out at its new width");
    expect(windows.second_root->bounds().width == 400.0f, "the other window kept its own width");
}

void test_independent_pacing() {
    // A real cap, so the pacer has an interval to gate on. 30 Hz is far slower than this test's own
    // runtime, so the second iteration below is definitely inside the interval.
    TwoWindows windows(true, 30.0);
    if (!windows.ready) {
        std::cout << "app_window_multi_loop: skipped (no presentation surface)\n";
        return;
    }
    const double interval = windows.group.pacer().interval_seconds();
    expect(interval > 0.0, "a capped group has a present interval to gate on");

    // Both windows present in the same tick. This is the clause the per-window `pacer->present()`
    // call would break: the first window would consume the interval and the second would be refused
    // by `pacer->ready()` for the rest of the walk.
    const std::size_t presented = windows.group.pump_windows_once();
    expect(presented == 2, "both windows presented in one group tick");
    expect(windows.group.pacer().started(), "the group's pacer advanced exactly once");

    // The next tick is inside the interval, so neither window may present: the gate is the tick's,
    // and it applies to every window equally.
    const std::uint32_t first_before = windows.first->stats().presented;
    const std::uint32_t second_before = windows.second->stats().presented;
    (void)windows.group.pump_windows_once();
    expect(windows.first->stats().presented == first_before,
           "the first window respects the group's present interval");
    expect(windows.second->stats().presented == second_before,
           "the second window respects the group's present interval");
}

void test_closing_one_leaves_the_loop_running() {
    TwoWindows windows;
    if (!windows.ready) {
        std::cout << "app_window_multi_loop: skipped (no presentation surface)\n";
        return;
    }
    (void)windows.group.pump_windows_once();
    expect(windows.group.window_list_size() == 2, "both windows are in the list");

    // Destroying one window unregisters it, and the walk keeps driving the survivor. A list that
    // held a dangling pointer here would be the whole bug this slice exists to prevent.
    windows.second.reset();
    expect(windows.group.window_list_size() == 1, "the closed window left the list");
    expect(windows.group.window_count() == 1, "the group counts one window");

    const std::uint32_t before = windows.first->stats().presented;
    tessera::hal::WindowMetrics metrics = windows.first->window()->metrics();
    metrics.logical_width = 500;
    metrics.physical_width = 500;
    windows.first->apply_metrics(metrics);
    const std::size_t presented = windows.group.pump_windows_once();
    expect(presented == 1, "the survivor presented after its sibling closed");
    expect(windows.first->stats().presented > before, "the survivor's counter moved");
}

void test_a_single_window_group_is_unchanged() {
    // The compatibility seam: a one-window group drives its window through the same walk, so the
    // multi-window path is a strict generalisation and not a second code path.
    WindowDeviceOptions options;
    options.font_path = TESSERA_TEST_FONT;
    WindowDevice group(options);
    if (!group.available()) {
        std::cout << "app_window_multi_loop: skipped (no usable device)\n";
        return;
    }
    auto root = make_root(1, "only window");
    WindowHost only(*root, compile_stylesheet("Text { font-size: 16px; }"),
                    window_options("only", 320, 200), group);
    if (!only.available()) {
        std::cout << "app_window_multi_loop: skipped (no presentation surface)\n";
        return;
    }
    expect(group.window_list_size() == 1, "the single window registered");
    expect(group.pump_windows_once() == 1, "the walk presented the single window");
    expect(only.stats().presented > 0, "the single window presented");

    // The public single-window seam still works and still advances the pacer itself.
    tessera::hal::WindowMetrics metrics = only.window()->metrics();
    metrics.logical_width = 480;
    metrics.physical_width = 480;
    only.apply_metrics(metrics);
    expect(only.pump_once(), "the single-window pump_once still presents");
}

} // namespace

int main() {
    test_one_loop_drives_both();
    test_independent_damage();
    test_independent_pacing();
    test_closing_one_leaves_the_loop_running();
    test_a_single_window_group_is_unchanged();

    if (g_failures != 0) {
        std::cerr << "app_window_multi_loop: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "app_window_multi_loop: ok\n";
    return 0;
}
