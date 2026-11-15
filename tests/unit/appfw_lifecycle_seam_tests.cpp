// SPDX-License-Identifier: MIT
//
// AF3 (issue #175) part 4: the driver seam.
//
// Two things are proven here, and they are the two halves the driver and the framework agree on:
//
//   1. `tessera::app::snapshot_lifecycle_events` - which transitions a `WindowMetrics` snapshot
//      implies, and in which order. This is the rule `WindowHost::Impl::apply_metrics_lifecycle`
//      calls, and it is pure, so it is proven without a window or a device;
//   2. `Application::make_lifecycle_sink()` - the adapter `WindowHost::set_lifecycle_sink` accepts.
//      The driver forwards `hal::WindowEvent` values into that interface, so replaying a window
//      event script through it is the same call path a real platform takes, minus the OS.
//
// Determinism: no window, no device, no clock, no thread. The events are values.

#include "tessera/app/lifecycle_events.hpp"
#include "tessera/app/lifecycle_sink.hpp"
#include "tessera/appfw/application.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"

#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using tessera::app::LifecycleBaseline;
using tessera::app::snapshot_lifecycle_events;
using tessera::appfw::app_state_name;
using tessera::appfw::Application;
using tessera::appfw::AppState;
using tessera::appfw::derive_app_state;
using tessera::appfw::ServiceRunner;
using tessera::hal::WindowEvent;
using tessera::hal::WindowEventType;
using tessera::hal::WindowMetrics;
using tessera::runtime::CompletionQueue;
using tessera::runtime::ManualClock;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_lifecycle: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

WindowMetrics snapshot(bool focused, bool occluded, bool minimized) {
    WindowMetrics metrics;
    metrics.focused = focused;
    metrics.occluded = occluded;
    metrics.minimized = minimized;
    return metrics;
}

// Collects the event types one snapshot produces, in order.
std::vector<WindowEventType> events_for(WindowMetrics metrics, LifecycleBaseline& baseline) {
    WindowEventType types[2]{};
    const std::size_t count = snapshot_lifecycle_events(metrics, baseline, types);
    return std::vector<WindowEventType>(types, types + count);
}

bool same(const std::vector<WindowEventType>& actual,
          const std::vector<WindowEventType>& expected) {
    return actual == expected;
}

void test_snapshot_rule() {
    // A window created visible and focused: its first snapshot is not a transition.
    {
        LifecycleBaseline baseline =
            tessera::app::lifecycle_baseline_for(snapshot(true, false, false));
        expect(events_for(snapshot(true, false, false), baseline).empty(),
               "the creation snapshot is not a transition");
    }
    // Hidden -> Restored, Occluded -> Restored, and the reverse, each exactly one event.
    {
        LifecycleBaseline baseline;
        expect(same(events_for(snapshot(true, true, false), baseline), {WindowEventType::Occluded}),
               "an occluded snapshot forwards Occluded");
    }
    // Occlusion outranks minimization: the same Hidden state is one transition, not two.
    {
        LifecycleBaseline baseline;
        expect(same(events_for(snapshot(true, true, true), baseline), {WindowEventType::Occluded}),
               "occlusion outranks minimization");
    }
    {
        LifecycleBaseline baseline;
        expect(
            same(events_for(snapshot(true, false, true), baseline), {WindowEventType::Minimized}),
            "a minimized snapshot forwards Minimized");
    }
    // Restore is Restored, and a repeated hidden snapshot forwards nothing more.
    {
        LifecycleBaseline baseline;
        expect(same(events_for(snapshot(true, true, false), baseline), {WindowEventType::Occluded}),
               "the first occluded snapshot forwards Occluded");
        expect(events_for(snapshot(true, true, false), baseline).empty(),
               "a repeated hidden snapshot forwards nothing");
        expect(
            same(events_for(snapshot(true, false, false), baseline), {WindowEventType::Restored}),
            "a visible snapshot forwards Restored");
    }
    // Visibility and focus are independent, and visibility comes first.
    {
        LifecycleBaseline baseline;
        expect(same(events_for(snapshot(false, true, false), baseline),
                    {WindowEventType::Occluded, WindowEventType::Blur}),
               "a hidden, blurred snapshot forwards Occluded then Blur");
    }
    {
        LifecycleBaseline baseline;
        expect(same(events_for(snapshot(false, false, false), baseline), {WindowEventType::Blur}),
               "a visible, blurred snapshot forwards Blur alone");
    }
    // Focus/blur never changes visibility: a blurred window is not Hidden.
    {
        WindowMetrics metrics = snapshot(false, false, false);
        LifecycleBaseline baseline = tessera::app::lifecycle_baseline_for(metrics);
        expect(!baseline.hidden, "a blurred window is not hidden");
        expect(baseline.focused == false, "the baseline records the lost focus");
    }
}

// The adapter the driver actually calls: one `LifecycleSink`, fed `hal::WindowEvent` values in the
// order a platform produced them, driving the real state machine.
void test_sink_adapter() {
    ManualClock clock;
    CompletionQueue queue{64};
    ServiceRunner runner(clock, queue, nullptr);
    Application application(runner);
    (void)application.launch();
    application.tick();
    auto sink = application.make_lifecycle_sink();
    expect(sink != nullptr, "the application produces a lifecycle sink");

    const auto forward = [&sink](WindowEventType type) {
        WindowEvent event;
        event.type = type;
        sink->on_window_lifecycle(event);
        return true;
    };

    // The ADR's desktop sequence, driven only through the sink: exactly the order a platform emits.
    const WindowEventType sequence[] = {
        WindowEventType::Blur,     WindowEventType::Focus,    WindowEventType::Minimized,
        WindowEventType::Restored, WindowEventType::Occluded, WindowEventType::Restored,
    };
    const AppState expected[] = {
        AppState::Inactive, AppState::Active, AppState::Hidden,
        AppState::Active,   AppState::Hidden, AppState::Active,
    };
    for (std::size_t index = 0; index < std::size(sequence); ++index) {
        expect(forward(sequence[index]), "the sink accepts a window event");
        expect(application.state() == expected[index],
               "the forwarded window event reaches the state machine in order");
        // The derivation agrees: the driver's event vocabulary and the ADR's table are one rule.
        expect(derive_app_state(application.window(), application.suspended()) ==
                   application.state(),
               "the sink's state matches the ADR table's derivation");
    }

    // An event that is not a lifecycle transition is dropped, not guessed at: a resize or a theme
    // change must not move the app state.
    const AppState before = application.state();
    (void)forward(WindowEventType::Resize);
    (void)forward(WindowEventType::ThemeChanged);
    (void)forward(WindowEventType::SafeAreaChanged);
    expect(application.state() == before, "a non-lifecycle window event moves nothing");

    // A close reaches the termination gate, and the app starts shutting down.
    (void)forward(WindowEventType::Close);
    expect(application.state() == AppState::Terminating, "a forwarded Close requests termination");
}

} // namespace

int run_lifecycle_seam_tests() {
    const int before = g_failures;
    test_snapshot_rule();
    test_sink_adapter();
    return g_failures - before;
}
