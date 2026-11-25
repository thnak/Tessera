// SPDX-License-Identifier: MIT
//
// Multi-window (issue #228, docs/design/multi-window.md): two windows present through one shared
// `WindowDevice`, closing one leaves the other running, and a resize or DPI change of one does not
// relayout the other.
//
// The test runs on whatever device the lane has. On a machine with no presentation surface the
// hosts report `available() == false` and the test prints `skipped`, which the CTest entry turns
// into a skip rather than a pass - a lane without a window backend proves nothing about this
// behaviour.
//
// What it asserts, and why each one is the acceptance rather than a proxy for it:
//
//   * **one device**: `device_for_testing()` returns the same object for both windows. Before this
//     slice each host created its own, so this is the refactor's whole point.
//   * **one renderer and one text stack**: the group reports a single presentation, and both
//   windows
//     draw through it. A second renderer would mean a second vertex ring and a second glyph atlas.
//   * **closing one leaves the other**: the surviving window still presents after its sibling is
//     destroyed, which is what proves the shared caches are not owned by a window.
//   * **resize isolation**: a metrics change on one window moves only that window's layout. The
//     other window's root keeps its own size, which is what "does not relayout the other" means.

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
        std::cerr << "app_window_multi: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// A root with one text label, so a frame has something to paint and the text stack is exercised.
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

void test_null_backend_shares_one_device() {
    // The acceptance's "on the Null RHI" half. The null backend records commands instead of
    // presenting, so it needs no surface and this case runs on a headless lane. What it proves is
    // the *device sharing* contract: two hosts created against one group see the same device
    // object, the group counts both, and closing one detaches without destroying the device.
    //
    // It deliberately does not assert a presented frame: the pipeline factories are
    // Vulkan-specific, so a null-backend group reports `available()` and then fails
    // `ensure_presentation` with a diagnostic rather than pretending to present. That split is
    // stated in `create_device`'s comment and is why the presentation half of this suite needs a
    // real device.
    WindowDeviceOptions device_options;
    device_options.font_path = TESSERA_TEST_FONT;
    device_options.backend = tessera::rendering::rhi::Backend::Null;
    WindowDevice group(device_options);
    if (!group.available()) {
        std::cout << "app_window_multi: skipped (no null device)\n";
        return;
    }
    expect(group.device_or_null() != nullptr, "the null group has a device");

    auto first_root = make_root(1, "first");
    auto second_root = make_root(10, "second");
    WindowHost first(*first_root, compile_stylesheet("Text { font-size: 16px; }"),
                     window_options("first", 320, 200), group);
    WindowHost second(*second_root, compile_stylesheet("Text { font-size: 16px; }"),
                      window_options("second", 400, 260), group);

    // The device is shared even when the presentation cannot be built: this is the assertion the
    // null lane exists for.
    expect(&first.device_for_testing() == &second.device_for_testing(),
           "both null-backend windows present through the same device");
    expect(&first.device_for_testing() == group.device_or_null(),
           "the windows' device is the group's device");
    expect(group.window_count() == 2, "the null group counts both windows");
}

void test_two_windows_share_one_device() {
    WindowDeviceOptions device_options;
    device_options.font_path = TESSERA_TEST_FONT;
    WindowDevice group(device_options);
    if (!group.available()) {
        std::cout << "app_window_multi: skipped (no usable device)\n";
        return;
    }

    auto first_root = make_root(1, "first window");
    auto second_root = make_root(10, "second window");
    WindowHost first(*first_root, compile_stylesheet("Text { font-size: 16px; }"),
                     window_options("first", 320, 200), group);
    WindowHost second(*second_root, compile_stylesheet("Text { font-size: 16px; }"),
                      window_options("second", 400, 260), group);

    if (!first.available() || !second.available()) {
        std::cout << "app_window_multi: skipped (no presentation surface)\n";
        return;
    }

    // The acceptance's first clause: one device, not two.
    expect(&first.device_for_testing() == &second.device_for_testing(),
           "both windows present through the same device");
    expect(group.window_count() == 2, "the group counts both windows");
    expect(group.has_presentation(), "the group created one shared presentation");

    // Both windows present. A shared renderer with a per-window slot gate must let each one paint.
    const auto first_stats = first.run(2);
    const auto second_stats = second.run(2);
    expect(first_stats.available, "the first window is available");
    expect(second_stats.available, "the second window is available");
    expect(first_stats.presented > 0, "the first window presented a frame");
    expect(second_stats.presented > 0, "the second window presented a frame");
    // The run reports memory: the OS answers on every lane this test runs on, and a window that
    // presented holds at least its glyph atlas or vertex buffer in the allocator's accounting.
    expect(first_stats.process_memory.valid && first_stats.process_memory.resident_bytes > 0,
           "a run reports the process's resident memory");
    expect(first_stats.gpu_memory.block_bytes >= first_stats.gpu_memory.allocation_bytes,
           "the allocator's blocks cover its allocations");
}

void test_closing_one_leaves_the_other() {
    WindowDeviceOptions device_options;
    device_options.font_path = TESSERA_TEST_FONT;
    WindowDevice group(device_options);
    if (!group.available()) {
        std::cout << "app_window_multi: skipped (no usable device)\n";
        return;
    }

    auto survivor_root = make_root(1, "survivor");
    WindowHost survivor(*survivor_root, compile_stylesheet("Text { font-size: 16px; }"),
                        window_options("survivor", 320, 200), group);
    if (!survivor.available()) {
        std::cout << "app_window_multi: skipped (no presentation surface)\n";
        return;
    }
    expect(survivor.run(1).presented > 0, "the survivor presented before the sibling existed");

    {
        auto doomed_root = make_root(10, "doomed");
        WindowHost doomed(*doomed_root, compile_stylesheet("Text { font-size: 16px; }"),
                          window_options("doomed", 320, 200), group);
        expect(doomed.available(), "the second window is available");
        expect(group.window_count() == 2, "both windows are attached");
        expect(doomed.run(1).presented > 0, "the second window presented");
    }
    // The sibling is gone; the group and its caches must still be there.
    expect(group.window_count() == 1, "the closed window detached from the group");
    expect(group.has_presentation(), "the shared presentation survived the close");
    expect(survivor.run(2).presented > 0, "the survivor still presents after its sibling closed");
}

void test_resize_does_not_relayout_the_other() {
    WindowDeviceOptions device_options;
    device_options.font_path = TESSERA_TEST_FONT;
    WindowDevice group(device_options);
    if (!group.available()) {
        std::cout << "app_window_multi: skipped (no usable device)\n";
        return;
    }

    auto first_root = make_root(1, "first");
    auto second_root = make_root(10, "second");
    WindowHost first(*first_root, compile_stylesheet("Text { font-size: 16px; }"),
                     window_options("first", 320, 200), group);
    WindowHost second(*second_root, compile_stylesheet("Text { font-size: 16px; }"),
                      window_options("second", 400, 260), group);
    if (!first.available() || !second.available()) {
        std::cout << "app_window_multi: skipped (no presentation surface)\n";
        return;
    }
    (void)first.run(1);
    (void)second.run(1);

    const float second_width_before = second_root->bounds().width;
    const float second_height_before = second_root->bounds().height;

    // Resize only the first window, through the same seam a platform resize uses.
    tessera::hal::WindowMetrics metrics = first.window()->metrics();
    metrics.logical_width = 640;
    metrics.logical_height = 400;
    metrics.physical_width = 640;
    metrics.physical_height = 400;
    first.apply_metrics(metrics);
    (void)first.run(1);

    // The first window re-laid out at its new size...
    expect(first_root->bounds().width > 320.0f, "the resized window re-laid out at its new width");
    // ...and the second did not move at all.
    expect(second_root->bounds().width == second_width_before,
           "the other window's width did not change");
    expect(second_root->bounds().height == second_height_before,
           "the other window's height did not change");
}

void test_a_second_window_adopts_the_group_rate() {
    WindowDeviceOptions device_options;
    device_options.font_path = TESSERA_TEST_FONT;
    WindowDevice group(device_options);
    if (!group.available()) {
        std::cout << "app_window_multi: skipped (no usable device)\n";
        return;
    }

    auto first_root = make_root(1, "first");
    WindowOptions first_options = window_options("first", 320, 200);
    first_options.max_fps = 30.0;
    WindowHost first(*first_root, compile_stylesheet("Text { font-size: 16px; }"), first_options,
                     group);
    if (!first.available()) {
        std::cout << "app_window_multi: skipped (no presentation surface)\n";
        return;
    }
    const double rate_after_first = group.pacer().interval_seconds();

    // A second window with a different cap must not reset the app's pacing: one app presents at one
    // rate (docs/design/multi-window.md section 5).
    auto second_root = make_root(10, "second");
    WindowOptions second_options = window_options("second", 320, 200);
    second_options.max_fps = 120.0;
    WindowHost second(*second_root, compile_stylesheet("Text { font-size: 16px; }"), second_options,
                      group);
    expect(group.pacer().interval_seconds() == rate_after_first,
           "a second window does not reset the group's pacing");
}

} // namespace

int main() {
    test_null_backend_shares_one_device();
    test_two_windows_share_one_device();
    test_closing_one_leaves_the_other();
    test_resize_does_not_relayout_the_other();
    test_a_second_window_adopts_the_group_rate();

    if (g_failures != 0) {
        std::cerr << "app_window_multi: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "app_window_multi: ok\n";
    return 0;
}
