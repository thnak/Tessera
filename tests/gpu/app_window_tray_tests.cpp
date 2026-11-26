// SPDX-License-Identifier: MIT
//
// Issue #181: a tray app runs with no visible window and zero presents, and shows a window on
// activation.
//
// The acceptance is two claims, and each case below is one clause of it rather than a proxy:
//
//   * **zero presents while hidden** - a hidden window acquires, paints and presents nothing. This
//     is the whole point of the slice: before it, `WindowOptions::visible = false` still painted
//     (the smoke test asserted `presented >= 3` on a hidden window), so a tray app would have
//     burned a GPU on a scene nobody could see.
//   * **a window on activation** - showing the window paints it, and the first frame after showing
//     is a full repaint, because nothing painted while hidden describes the screen.
//   * **a window-less group parks** - the group loop runs with no window at all, which is the tray
//     app's steady state: services and timers alive, nothing painted. Before this slice the only
//     loop was `WindowHost::run()`, which parks on *its own* window's event wait, so an app with no
//     window had no loop.
//
// The test runs on whatever device the lane has. On a machine with no presentation surface the
// hosts report `available() == false` and it prints `skipped`, which the CTest entry turns into a
// skip rather than a pass - a lane without a window backend proves nothing about this behaviour.

#include "tessera/app/window.hpp"
#include "tessera/app/window_device.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
        std::cerr << "app_window_tray: " << message << "\n";
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

WindowOptions window_options(std::string title, bool visible) {
    WindowOptions options;
    options.title = std::move(title);
    options.width = 320;
    options.height = 200;
    options.visible = visible;
    options.font_path = TESSERA_TEST_FONT;
    return options;
}

WindowDeviceOptions device_options() {
    WindowDeviceOptions options;
    options.font_path = TESSERA_TEST_FONT;
    return options;
}

// A group plus one window. `visible` is the *creation* state: false is the offscreen/CI pattern,
// which still paints. The tray app's zero-present state comes from `set_visible(false)` instead.
struct TrayWindow {
    explicit TrayWindow(bool visible = true)
        : group(device_options()) {
        if (!group.available()) {
            return;
        }
        root = make_root(1, "tray window");
        host = std::make_unique<WindowHost>(*root, compile_stylesheet("Text { font-size: 16px; }"),
                                            window_options("tray", visible), group);
        ready = host->available();
    }

    WindowDevice group;
    std::unique_ptr<Node> root;
    std::unique_ptr<WindowHost> host;
    bool ready = false;
};

void test_a_hidden_window_presents_nothing() {
    TrayWindow window;
    if (!window.ready) {
        std::cout << "app_window_tray: skipped (no presentation surface)\n";
        return;
    }
    // The app hides its window: this is the tray app's steady state, and it is the app's own
    // decision rather than a creation-time flag.
    window.host->set_visible(false);
    expect(!window.host->visible(), "the window reports itself hidden after hiding");

    // Several iterations, not one: a single no-op could be a startup coincidence, and the bug this
    // slice fixes was that a hidden window painted on *every* iteration.
    for (int iteration = 0; iteration < 4; ++iteration) {
        (void)window.group.pump_windows_once();
    }
    expect(window.host->stats().presented == 0, "a hidden window presented nothing");
    expect(window.host->stats().frames == 0, "a hidden window painted no frame");
    // The pause is what produces the zero: the iteration ran and was counted as paused, so this is
    // not a window that simply never got a turn.
    expect(window.host->stats().paused_iterations > 0,
           "the hidden iterations were counted as paused");
}

void test_an_offscreen_window_still_paints() {
    // The compatibility seam, and the reason hiding is a separate flag from the creation state: a
    // window created with `visible = false` is the offscreen/CI pattern, where the app paints into
    // a window the user never sees. It must keep presenting, or every offscreen capture and every
    // bounded CI run would stop producing frames.
    TrayWindow window(false);
    if (!window.ready) {
        std::cout << "app_window_tray: skipped (no presentation surface)\n";
        return;
    }
    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented > 0,
           "a window created offscreen still presents (the CI/offscreen pattern)");
    expect(window.host->visible(), "an offscreen window is not hidden by the app");
}

void test_showing_the_window_paints_it() {
    TrayWindow window;
    if (!window.ready) {
        std::cout << "app_window_tray: skipped (no presentation surface)\n";
        return;
    }
    window.host->set_visible(false);
    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented == 0, "nothing is painted before activation");

    // Activation: the tray item shows the window.
    window.host->set_visible(true);
    expect(window.host->visible(), "the window reports itself visible after activation");

    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented > 0, "the activated window painted a frame");
    // The first frame after showing must cover the whole target: nothing painted while hidden
    // describes the screen, so a damage-only repaint could leave stale pixels.
    expect(window.host->stats().full_repaints > 0,
           "the first frame after showing is a full repaint");
}

void test_hiding_again_stops_presenting() {
    TrayWindow window;
    if (!window.ready) {
        std::cout << "app_window_tray: skipped (no presentation surface)\n";
        return;
    }
    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented > 0, "a visible window paints");
    const std::uint32_t before = window.host->stats().presented;

    // Hiding is not closing: the window keeps its scene, its swapchain and its place in the group,
    // so showing it again is one repaint rather than a rebuild.
    window.host->set_visible(false);
    expect(!window.host->visible(), "the window reports itself hidden after hiding");
    expect(window.group.window_list_size() == 1, "a hidden window stays in the group");

    // Damage the scene while hidden: the change must not produce a present.
    tessera::hal::WindowMetrics metrics = window.host->window()->metrics();
    metrics.logical_width = 480;
    metrics.physical_width = 480;
    window.host->apply_metrics(metrics);
    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented == before,
           "a hidden window presents nothing after damage");

    // Showing it again paints the change that accumulated while it was hidden.
    window.host->set_visible(true);
    (void)window.group.pump_windows_once();
    expect(window.host->stats().presented > before, "showing the window paints the pending change");
}

void test_a_window_less_group_parks() {
    // The tray app's steady state: the app is running, its services and timers are alive, and there
    // is no window at all. Before this slice there was no loop that could run in that state.
    WindowDevice group(device_options());
    if (!group.available()) {
        std::cout << "app_window_tray: skipped (no usable device)\n";
        return;
    }
    expect(group.window_list_size() == 0, "a fresh group has no window");

    // The group still knows when it next has work, which is what a caller parks on.
    const double wait = group.next_wait_seconds();
    expect(wait >= 0.0, "a window-less group reports a non-negative wait");

    // A bounded run with nothing to paint returns at once instead of parking for frames that cannot
    // happen.
    const WindowDevice::RunStats stats = group.run_windows(3);
    expect(stats.presented == 0, "a window-less bounded run presented nothing");
    expect(stats.iterations == 0, "a window-less bounded run did not iterate");

    // An unbounded run is stopped by request_stop(), which is the only way a window-less run can
    // end: there is no window whose close would stop it.
    group.request_stop();
    const WindowDevice::RunStats stopped = group.run_windows(0);
    expect(stopped.stopped, "request_stop() ended the window-less run");
    expect(stopped.presented == 0, "the stopped run presented nothing");
}

void test_a_worker_completion_wakes_a_parked_window_less_group() {
    // Issue #842 on the tray path: an idle group with no timer parks at the idle clamp (250 ms).
    // A worker's completion has to end that park, or every async result in a tray app is late.
    WindowDevice group(device_options());
    if (!group.available()) {
        std::cout << "app_window_tray: skipped (no usable device)\n";
        return;
    }
    tessera::runtime::CompletionQueue completions;
    group.loop().set_completion_queue(&completions);
    group.set_completion_wake(&completions);

    std::atomic<int> ran{0};
    std::thread worker([&completions, &group, &ran]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        // A burst: the three posts are one empty -> non-empty edge, so one wake.
        for (int post = 0; post < 3; ++post) {
            completions.post(tessera::runtime::CompletionPriority::Visible, [&group, &ran]() {
                ++ran;
                group.request_stop(); // runs on the UI thread, at the tick that drains the queue
            });
        }
    });
    const double begin = tessera::hal::steady_now_seconds();
    const WindowDevice::RunStats stats = group.run_windows(0);
    const double elapsed = tessera::hal::steady_now_seconds() - begin;
    worker.join();
    group.set_completion_wake(nullptr);
    group.loop().set_completion_queue(nullptr);

    expect(stats.stopped, "the completion ran on the UI thread and stopped the run");
    expect(ran.load() == 3, "the burst's completions all ran");
    expect(stats.presented == 0, "a window-less group presented nothing");
    expect(stats.idle_waits == 1 && stats.iterations < 100,
           "a window-less group parks instead of spinning");
    expect(elapsed < 0.15, "the worker's post ended the park well before the 250 ms clamp");
}

void test_a_tray_app_activates_into_a_window() {
    // The whole acceptance in one sequence: start with no window, run the group loop, attach a
    // window on activation, and paint it.
    WindowDevice group(device_options());
    if (!group.available()) {
        std::cout << "app_window_tray: skipped (no usable device)\n";
        return;
    }
    expect(group.window_list_size() == 0, "the tray app starts with no window");

    // Activation: the app creates its window. This is what a tray item's click action does.
    auto root = make_root(1, "activated");
    WindowHost host(*root, compile_stylesheet("Text { font-size: 16px; }"),
                    window_options("activated", true), group);
    if (!host.available()) {
        std::cout << "app_window_tray: skipped (no presentation surface)\n";
        return;
    }
    expect(group.window_list_size() == 1, "the activated window joined the group");

    const std::size_t presented = group.pump_windows_once();
    expect(presented == 1, "the group painted the activated window");
    expect(host.stats().presented > 0, "the activated window presented");
}

} // namespace

int main() {
    test_a_hidden_window_presents_nothing();
    test_an_offscreen_window_still_paints();
    test_showing_the_window_paints_it();
    test_hiding_again_stops_presenting();
    test_a_window_less_group_parks();
    test_a_worker_completion_wakes_a_parked_window_less_group();
    test_a_tray_app_activates_into_a_window();

    if (g_failures != 0) {
        std::cerr << "app_window_tray: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "app_window_tray: ok\n";
    return 0;
}
