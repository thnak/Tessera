// SPDX-License-Identifier: MIT
//
// Issue #180 part 1: the app's window set and the ADR-050 section 2 aggregation over it.
//
// The acceptance is "two windows with independent lifecycle". That is two claims, and this file
// proves the first one headlessly:
//
//   1. **independent lifecycle** - a signal folded into one window moves only that window, and the
//      app state is derived from the aggregate over all of them. The table below is written out by
//      hand from ADR-050 section 2's "desktop source" column, so a change in the aggregation is a
//      test failure rather than a silently different but still "passing" run.
//   2. **the last window is the one that matters** - closing one of two windows leaves the app
//      running on the survivor; closing the last one is the termination request.
//
// Determinism: no window, no device, no clock, no thread. The registry is a table of booleans and
// the aggregation is a pure function, which is exactly why the rule can be asserted row by row
// instead of only end to end through a platform.

#include "appfw_test_services.hpp"
#include "tessera/appfw/application.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/appfw/window_registry.hpp"
#include "tessera/runtime/clock.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

using tessera::appfw::aggregate_window_lifecycle;
using tessera::appfw::Application;
using tessera::appfw::ApplicationConfig;
using tessera::appfw::AppState;
using tessera::appfw::derive_app_state;
using tessera::appfw::kMaxWindowsPerApplication;
using tessera::appfw::kPrimaryWindow;
using tessera::appfw::LifecycleEvent;
using tessera::appfw::LifecycleSignal;
using tessera::appfw::ServiceRunner;
using tessera::appfw::WindowId;
using tessera::appfw::WindowLifecycle;
using tessera::appfw::WindowRegistry;
using tessera::runtime::CompletionQueue;
using tessera::runtime::ManualClock;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_window_registry: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// A visible, focused, open window: the aggregate's `Active` row.
constexpr WindowLifecycle kVisibleFocused{true, true, false, false, false};
// A visible, unfocused, open window: the `Inactive` row.
constexpr WindowLifecycle kVisibleUnfocused{true, false, false, false, false};
// An open but hidden window (minimized): the `Hidden` row.
constexpr WindowLifecycle kMinimized{true, true, true, false, false};
// A closed window: also the `Hidden` row, and the one `open_count` excludes.
constexpr WindowLifecycle kClosed{true, true, false, false, true};

// --- the aggregation table ---------------------------------------------------------------------

void test_aggregation_table() {
    // The empty set is `closed`: an app with no window has no visible window, which is the tray-app
    // row of ADR-050 section 2 (`quitOnLastWindowClosed = false`).
    {
        const WindowLifecycle aggregate = aggregate_window_lifecycle({});
        expect(aggregate.closed, "an empty window set is closed");
        expect(!aggregate.visible, "an empty window set has no visible window");
        expect(derive_app_state(aggregate, false) == AppState::Hidden,
               "an empty window set derives Hidden");
    }

    // One window: the aggregate is that window, so the N-window rule is a strict generalisation of
    // the one-window rule AF3 landed.
    {
        const WindowLifecycle one[] = {kVisibleFocused};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(one);
        expect(aggregate.visible && aggregate.focused && !aggregate.closed,
               "one visible focused window aggregates to visible and focused");
        expect(derive_app_state(aggregate, false) == AppState::Active,
               "one visible focused window derives Active");
    }
    {
        const WindowLifecycle one[] = {kVisibleUnfocused};
        expect(derive_app_state(aggregate_window_lifecycle(one), false) == AppState::Inactive,
               "one visible unfocused window derives Inactive");
    }
    {
        const WindowLifecycle one[] = {kMinimized};
        expect(derive_app_state(aggregate_window_lifecycle(one), false) == AppState::Hidden,
               "one minimized window derives Hidden");
    }

    // Two windows, the ADR's own phrases:
    //
    //   "a window focused"  -> Active, even when the other window is blurred
    //   "all windows blurred" -> Inactive
    //   "no visible window" -> Hidden
    {
        const WindowLifecycle both[] = {kVisibleFocused, kVisibleUnfocused};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(both);
        expect(aggregate.focused, "one focused window makes the set focused");
        expect(derive_app_state(aggregate, false) == AppState::Active,
               "a focused window among blurred ones derives Active");
    }
    {
        const WindowLifecycle both[] = {kVisibleUnfocused, kVisibleUnfocused};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(both);
        expect(!aggregate.focused, "all windows blurred makes the set unfocused");
        expect(derive_app_state(aggregate, false) == AppState::Inactive,
               "all windows blurred derives Inactive");
    }
    {
        // One minimized, one visible and focused: the app is Active, because a window is on screen.
        // This is the row a per-window state machine gets wrong by folding the two together.
        const WindowLifecycle both[] = {kMinimized, kVisibleFocused};
        expect(derive_app_state(aggregate_window_lifecycle(both), false) == AppState::Active,
               "a visible window outranks a minimized sibling");
    }
    {
        // Both hidden: the app is Hidden, and `minimized` records why.
        const WindowLifecycle both[] = {kMinimized, kMinimized};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(both);
        expect(!aggregate.visible, "two minimized windows are not visible");
        expect(aggregate.minimized, "the aggregate records that the set is minimized");
        expect(!aggregate.closed, "two minimized windows are still open");
        expect(derive_app_state(aggregate, false) == AppState::Hidden,
               "two minimized windows derive Hidden");
    }
    {
        // A closed window is not visible even when its stale flags still say so: the platform may
        // report a focus for a window the app has already closed.
        const WindowLifecycle stale{true, true, false, false, true};
        const WindowLifecycle both[] = {stale, kVisibleUnfocused};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(both);
        expect(aggregate.visible, "the open window keeps the set visible");
        expect(!aggregate.focused, "a closed window's stale focus does not count");
        expect(derive_app_state(aggregate, false) == AppState::Inactive,
               "a closed focused window does not make the app Active");
    }
    {
        // Every window closed: the tray-app row.
        const WindowLifecycle both[] = {kClosed, kClosed};
        const WindowLifecycle aggregate = aggregate_window_lifecycle(both);
        expect(aggregate.closed, "a set of closed windows is closed");
        expect(derive_app_state(aggregate, false) == AppState::Hidden,
               "a set of closed windows derives Hidden");
    }

    // Suspension outranks the window set, exactly as it outranks one window.
    {
        const WindowLifecycle both[] = {kVisibleFocused, kVisibleUnfocused};
        expect(derive_app_state(aggregate_window_lifecycle(both), true) == AppState::Suspended,
               "suspension outranks the window set");
    }
}

// --- the registry table ------------------------------------------------------------------------

void test_registry_attach_and_detach() {
    WindowRegistry registry;
    expect(registry.empty(), "a fresh registry is empty");
    expect(registry.aggregate().closed, "a fresh registry aggregates to closed");

    const auto first = registry.attach(kVisibleFocused);
    const auto second = registry.attach(kVisibleUnfocused);
    expect(first.has_value() && second.has_value(), "two windows attach");
    expect(first != second, "each window gets its own id");
    expect(registry.size() == 2, "the registry counts both windows");
    expect(registry.open_count() == 2, "both windows are open");

    // The ids are stable and address their own window: this is the "independent lifecycle" claim at
    // the table level.
    expect(registry.find(*first) != nullptr && registry.find(*first)->focused,
           "the first window is the focused one");
    expect(registry.find(*second) != nullptr && !registry.find(*second)->focused,
           "the second window is the unfocused one");

    // Folding one window moves only that window.
    expect(registry.fold(*second, LifecycleSignal::Focus), "the second window folds a Focus");
    expect(registry.find(*second)->focused, "the second window became focused");
    expect(registry.find(*first)->focused, "the first window is untouched by the second's event");

    // Detach preserves attach order and never recycles an id.
    expect(registry.detach(*first), "the first window detaches");
    expect(registry.size() == 1, "one window remains");
    expect(registry.find(*first) == nullptr, "the detached window is gone");
    expect(!registry.detach(*first), "detaching twice is refused");
    const auto third = registry.attach(kVisibleFocused);
    expect(third.has_value(), "a third window attaches");
    expect(*third != *first, "a detached window's id is never reused");

    // A full table refuses rather than growing: the bound is a real capacity.
    WindowRegistry small(1);
    expect(small.attach().has_value(), "the first window fits");
    expect(!small.attach().has_value(), "a full registry refuses another window");
    expect(small.capacity() == 1, "the capacity is what was asked for");
}

void test_registry_snapshot() {
    WindowRegistry registry;
    const auto id = registry.attach(kVisibleFocused);
    expect(id.has_value(), "the window attaches");

    // A snapshot that hides the window is the metrics channel a platform without an occlusion event
    // uses, so it must fold exactly like the event would.
    expect(registry.apply_snapshot(*id, true, true, false), "the snapshot applies");
    expect(registry.find(*id)->occluded, "the snapshot recorded the occlusion");
    expect(!registry.find(*id)->visible, "an occluded window is not visible");
    expect(registry.aggregate().minimized, "the aggregate reports the set as hidden");

    // A visible, focused snapshot clears the closed bit, the same rule one window had.
    (void)registry.fold(*id, LifecycleSignal::Close);
    expect(registry.find(*id)->closed, "the window closed");
    expect(registry.open_count() == 0, "no window is open");
    (void)registry.apply_snapshot(*id, true, false, false);
    expect(!registry.find(*id)->closed, "a visible focused snapshot reopens the window");
    expect(registry.open_count() == 1, "the window is open again");

    // An unknown id is refused, not silently ignored.
    expect(!registry.apply_snapshot(WindowId{9999}, true, false, false),
           "a snapshot for an unknown window is refused");
    expect(!registry.fold(WindowId{9999}, LifecycleSignal::Focus),
           "a fold for an unknown window is refused");
}

// --- the application's window set --------------------------------------------------------------

// A runner with no services: the state machine is what is under test, not the service lifecycle.
struct Fixture {
    Fixture(ApplicationConfig config = {})
        : runner(clock, queue, nullptr),
          application(runner, config) {
        expect(runner.prepare().has_value(), "the runner prepares");
        expect(application.launch(), "the application launches");
        application.tick();
    }

    ManualClock clock;
    CompletionQueue queue{64};
    ServiceRunner runner;
    Application application;
};

void test_application_window_set() {
    Fixture fixture;
    // The primary window exists from construction, so a single-window app is unchanged.
    expect(fixture.application.windows().size() == 1, "the primary window is registered");
    expect(fixture.application.window().focused, "the primary window starts focused");
    expect(fixture.application.state() == AppState::Active, "the app starts Active");

    const auto second =
        fixture.application.attach_window(WindowLifecycle{true, false, false, false, false});
    expect(second.has_value(), "a second window attaches");
    expect(fixture.application.windows().size() == 2, "the app tracks both windows");

    // Blurring the primary window does not make the app Inactive while the second is focused.
    (void)fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Focus});
    (void)fixture.application.dispatch(kPrimaryWindow, LifecycleEvent{LifecycleSignal::Blur});
    expect(fixture.application.state() == AppState::Active,
           "a focused second window keeps the app Active");

    // Blurring both is the ADR's "all windows blurred" row.
    (void)fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Blur});
    expect(fixture.application.state() == AppState::Inactive,
           "all windows blurred derives Inactive");

    // Minimizing both is "no visible window".
    (void)fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Minimize});
    (void)fixture.application.dispatch(kPrimaryWindow, LifecycleEvent{LifecycleSignal::Minimize});
    expect(fixture.application.state() == AppState::Hidden, "all windows minimized derives Hidden");

    // Restoring one is enough to leave Hidden.
    (void)fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Restore});
    expect(fixture.application.state() == AppState::Inactive,
           "restoring one window leaves Hidden for the state the set derives");
}

void test_closing_one_of_two_keeps_the_app() {
    Fixture fixture;
    const auto second = fixture.application.attach_window();
    expect(second.has_value(), "a second window attaches");

    // Closing one of two windows is not a termination request: the survivor keeps the app running.
    // This is the acceptance's "independent lifecycle" in the form a user sees it.
    const auto result =
        fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Close});
    expect(result.state == AppState::Active, "closing one of two windows leaves the app Active");
    expect(fixture.application.state() != AppState::Terminating,
           "closing one of two windows does not terminate the app");
    expect(fixture.application.windows().open_count() == 1, "one window is still open");

    // Closing the last one is the ADR's termination request.
    (void)fixture.application.dispatch(kPrimaryWindow, LifecycleEvent{LifecycleSignal::Close});
    expect(fixture.application.state() == AppState::Terminating,
           "closing the last window requests termination");
}

void test_tray_app_stays_alive_with_no_window() {
    ApplicationConfig config;
    config.quit_on_last_window_closed = false;
    Fixture fixture(config);

    const auto second = fixture.application.attach_window();
    expect(second.has_value(), "a second window attaches");
    (void)fixture.application.dispatch(*second, LifecycleEvent{LifecycleSignal::Close});
    (void)fixture.application.dispatch(kPrimaryWindow, LifecycleEvent{LifecycleSignal::Close});
    expect(fixture.application.state() == AppState::Hidden,
           "with quitOnLastWindowClosed off, closing every window is Hidden");
    expect(fixture.application.windows().open_count() == 0, "no window is open");
    expect(fixture.application.aggregate_window().closed, "the aggregate is closed");

    // A focus reopens the app: the platform is showing a window again.
    (void)fixture.application.dispatch(kPrimaryWindow, LifecycleEvent{LifecycleSignal::Focus});
    expect(fixture.application.state() == AppState::Active,
           "a focused window brings a tray app back");
}

void test_per_window_lifecycle_sink() {
    Fixture fixture;
    const auto second = fixture.application.attach_window();
    expect(second.has_value(), "a second window attaches");

    // One sink per window: the driver installs one on each host, so each window's events fold into
    // its own slot instead of into one shared aggregate.
    auto primary_sink = fixture.application.make_lifecycle_sink();
    auto second_sink = fixture.application.make_lifecycle_sink(*second);
    expect(primary_sink != nullptr && second_sink != nullptr, "each window gets a sink");

    tessera::hal::WindowEvent blur;
    blur.type = tessera::hal::WindowEventType::Blur;
    second_sink->on_window_lifecycle(blur);
    expect(fixture.application.state() == AppState::Active,
           "blurring the second window leaves the focused primary window Active");

    primary_sink->on_window_lifecycle(blur);
    expect(fixture.application.state() == AppState::Inactive,
           "blurring both windows derives Inactive");

    // A close through one window's sink is not a termination request while the other is open.
    tessera::hal::WindowEvent close;
    close.type = tessera::hal::WindowEventType::Close;
    second_sink->on_window_lifecycle(close);
    expect(fixture.application.state() != AppState::Terminating,
           "a close through one window's sink does not terminate the app");
}

void test_window_capacity_is_bounded() {
    ApplicationConfig config;
    config.max_windows = 2;
    Fixture fixture(config);
    expect(fixture.application.windows().capacity() == 2, "the configured capacity is honoured");
    expect(fixture.application.attach_window().has_value(), "the second window fits");
    expect(!fixture.application.attach_window().has_value(),
           "a full window set refuses another window");
    expect(kMaxWindowsPerApplication >= 2, "the default bound covers the two-window case");
}

} // namespace

int run_window_registry_tests() {
    const int before = g_failures;
    test_aggregation_table();
    test_registry_attach_and_detach();
    test_registry_snapshot();
    test_application_window_set();
    test_closing_one_of_two_keeps_the_app();
    test_tray_app_stays_alive_with_no_window();
    test_per_window_lifecycle_sink();
    test_window_capacity_is_bounded();
    return g_failures - before;
}
