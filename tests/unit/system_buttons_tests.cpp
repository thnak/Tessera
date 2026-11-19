// SPDX-License-Identifier: MIT
//
// System buttons as normalized events (docs/specs/studio-device-profiles-v1.md sections 1 and 4,
// issue #445): Back and Volume input events, the Home/Recents/Power mappings, the unhandled-Back
// result, and the saved-state record. Everything runs over `ManualClock` and the synthetic
// injector: no wall clock, no OS, no window.

#include "appfw_navigation_test_support.hpp"
#include "overlay_test_fixture.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/saved_state.hpp"
#include "tessera/hal/synthetic_events.hpp"
#include "tessera/ui/input_pump.hpp"

#include <vector>

namespace {

using namespace overlay_test;
using tessera::app::BackDispatch;
using tessera::appfw::AppState;
using tessera::appfw::LifecycleSignal;
using tessera::appfw::SavedStateRecord;
using tessera::appfw::SavedStateRecorder;
using tessera::hal::OsEvent;
using tessera::hal::OsEventType;
using tessera::hal::SyntheticEventInjector;
using tessera::hal::SystemButton;
using tessera::ui::InputPump;
using tessera::ui::VolumeDirection;

void check(bool condition, const char* message) {
    expect(condition, message);
}

// The pump with the host's overlay input hook installed, as the window driver wires it.
struct Driver {
    explicit Driver(Fixture& fixture)
        : fixture(fixture) {
        pump.set_input_filter(
            [](void* user, Node&, const InputEvent& event) {
                return static_cast<tessera::app::AppHost*>(user)->handle_overlay_input(event);
            },
            fixture.host.get());
    }

    // Delivers one injected input event the way the window would: through the pump's queue.
    void deliver(SyntheticEventInjector& injector) {
        // One event per call: `feed_input` would move every pending event at once.
        InputEvent event;
        check(injector.pop_input(event), "an input event was pending");
        pump.queue().push(event);
        pump.pump(fixture.root, fixture.loop);
        fixture.frame();
    }

    Fixture& fixture;
    InputPump pump;
};

// --- Back ---------------------------------------------------------------------------------------

void back_pops_overlay_then_route_then_reports_unhandled() {
    Fixture f;
    Driver driver(f);
    nav_test::Harness nav;
    static_cast<void>(nav.controller.push("/"));
    static_cast<void>(nav.controller.push("/orders"));
    nav.controller.commit();
    f.host->set_back_handler(&tessera::appfw::nav_back_handler, &nav.controller);

    f.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    f.frame();
    check(f.host->overlays().size() == 1, "the menu is open");

    // The script is back-pop-back-exit: three identical Back events; the app state decides what
    // each one does.
    SyntheticEventInjector script = tessera::hal::make_back_pop_back_exit_script();
    check(script.pending_input() == 3, "the script holds three Back presses");
    check(script.empty(), "and no OS event");

    {
        // First Back: the topmost dismissible overlay, the route stack untouched.
        InputEvent first;
        check(script.pop_input(first) && first.type == InputEventType::BackRequested,
              "the first event is BackRequested");
        check(first.timestamp_s == 0.0, "stamped by the simulated clock");
        driver.pump.queue().push(first);
        driver.pump.pump(f.root, f.loop);
        f.frame();
        check(f.host->last_back_dispatch() == BackDispatch::OverlayDismissed,
              "Back dismisses the overlay first");
        check(f.host->overlays().size() == 0, "the overlay is gone");
        check(nav.controller.depth() == 2, "and the route stack was not touched");
    }

    // Second Back: no overlay, so the route is popped.
    driver.deliver(script);
    check(f.host->last_back_dispatch() == BackDispatch::HandlerConsumed, "then a route is popped");
    check(nav.controller.depth() == 1 && nav.controller.active()->route_id == "home",
          "the previous route is active and published");
    check(f.host->unhandled_back_count() == 0, "nothing was unhandled so far");

    // Third Back: at the root nothing pops, so it is reported unhandled.
    driver.deliver(script);
    check(f.host->last_back_dispatch() == BackDispatch::Unhandled, "then Back is unhandled");
    check(f.host->unhandled_back_count() == 1, "and the host can count it");
    check(nav.controller.depth() == 1, "the root is never popped");
}

void repeated_back_on_an_empty_stack_stays_unhandled() {
    Fixture f;
    Driver driver(f);
    nav_test::Harness nav;
    check(nav.controller.depth() == 0, "the controller has no route");
    f.host->set_back_handler(&tessera::appfw::nav_back_handler, &nav.controller);

    SyntheticEventInjector injector;
    for (int index = 0; index < 5; ++index) {
        static_cast<void>(injector.press(SystemButton::Back));
        driver.deliver(injector);
        check(f.host->last_back_dispatch() == BackDispatch::Unhandled, "Back stays unhandled");
    }
    check(f.host->unhandled_back_count() == 5, "every press is counted, none accumulates state");
    check(nav.controller.depth() == 0 && !nav.controller.pending(), "the stack is untouched");
}

void back_without_an_app_handler_is_overlay_or_unhandled() {
    Fixture f;
    Driver driver(f);
    SyntheticEventInjector injector;
    static_cast<void>(injector.press(SystemButton::Back));
    driver.deliver(injector);
    check(f.host->last_back_dispatch() == BackDispatch::Unhandled,
          "no overlay and no handler: unhandled");
}

// --- the mappings and the section 3 rules -------------------------------------------------------

std::vector<OsEventType> drain_types(SyntheticEventInjector& injector) {
    std::vector<OsEventType> types;
    OsEvent event;
    while (injector.pop(event)) {
        types.push_back(event.type);
    }
    return types;
}

void buttons_map_onto_existing_events() {
    SyntheticEventInjector home;
    static_cast<void>(home.press(SystemButton::Home));
    const std::vector<OsEventType> home_events = drain_types(home);
    check(home_events == std::vector<OsEventType>{OsEventType::WindowHidden, OsEventType::Suspend},
          "Home is WindowHidden then Suspend");

    SyntheticEventInjector recents;
    static_cast<void>(recents.press(SystemButton::Recents));
    check(drain_types(recents) == std::vector<OsEventType>{OsEventType::WindowDeactivated},
          "Recents is WindowDeactivated");
    check(recents.pending_input() == 0, "and no input event");

    SyntheticEventInjector power = tessera::hal::make_power_cycle_script();
    check(drain_types(power) ==
              std::vector<OsEventType>{OsEventType::ScreenOff, OsEventType::ScreenOn},
          "Power is ScreenOff, then ScreenOn on wake");
    check(power.screen_on(), "the screen is on again after the cycle");

    SyntheticEventInjector volume;
    static_cast<void>(volume.press(SystemButton::VolumeUp));
    static_cast<void>(volume.press(SystemButton::VolumeDown));
    check(volume.empty(), "Volume is not an OS event");
    InputEvent up;
    InputEvent down;
    check(volume.pop_input(up) && up.type == InputEventType::VolumeKey &&
              up.volume_direction == VolumeDirection::Up,
          "VolumeUp is a VolumeKey up");
    check(volume.pop_input(down) && down.volume_direction == VolumeDirection::Down,
          "VolumeDown is a VolumeKey down");
}

void a_full_home_is_refused_whole() {
    SyntheticEventInjector injector;
    for (std::size_t index = 0; index + 1 < tessera::hal::kMaxSyntheticEvents; ++index) {
        static_cast<void>(injector.push(OsEventType::Resume));
    }
    check(injector.press(SystemButton::Home) == tessera::hal::InjectStatus::Full,
          "Home needs two slots");
    check(injector.pending() == tessera::hal::kMaxSyntheticEvents - 1,
          "and delivered neither half");
}

void ordering_and_coalescing_follow_os_events_section_3() {
    // Pressed out of order on purpose: Back first, the OS events after.
    SyntheticEventInjector injector;
    static_cast<void>(injector.press(SystemButton::Back));
    static_cast<void>(injector.press(SystemButton::Home));
    static_cast<void>(injector.press(SystemButton::VolumeUp));
    static_cast<void>(injector.press(SystemButton::Back));

    // One tick delivers OS events first, then input, in queue order: the fixed order of section 3.
    std::vector<int> delivered; // 0 = OS event, 1 = input
    OsEvent os;
    while (injector.pop(os)) {
        delivered.push_back(0);
    }
    InputEvent input;
    std::vector<InputEventType> inputs;
    while (injector.pop_input(input)) {
        delivered.push_back(1);
        inputs.push_back(input.type);
    }
    check((delivered == std::vector<int>{0, 0, 1, 1, 1}), "lifecycle events precede input");
    check((inputs == std::vector<InputEventType>{InputEventType::BackRequested,
                                                 InputEventType::VolumeKey,
                                                 InputEventType::BackRequested}),
          "input keeps queue order and Back/Volume are never merged");

    // Resize coalesces, lifecycle never does: two Homes keep all four events.
    SyntheticEventInjector storm;
    for (int step = 0; step < 4; ++step) {
        static_cast<void>(storm.push(OsEventType::WindowResized));
    }
    check(storm.pending() == 1 && storm.coalesced() == 3, "a resize storm collapses to one");
    static_cast<void>(storm.press(SystemButton::Home));
    static_cast<void>(storm.press(SystemButton::Home));
    check(storm.pending() == 5, "two Home presses keep all four lifecycle events");
    check(storm.coalesced() == 3, "nothing system-button was merged");
}

void volume_reaches_the_focused_node_and_back_reaches_no_node() {
    struct Recorder : Node {
        using Node::Node;
        void on_event(tessera::ui::Event& event) override {
            events.push_back(event);
        }
        std::vector<tessera::ui::Event> events;
    };
    Recorder root(1);
    root.set_bounds({0, 0, 100, 100});
    auto child_ptr = std::make_unique<Recorder>(2);
    Recorder* child = child_ptr.get();
    child->set_bounds({0, 0, 50, 50});
    child->set_focusable(true);
    root.add_child(std::move(child_ptr));

    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    InputPump pump;
    SyntheticEventInjector injector;

    // No focus yet: the volume key has no target and is dropped, never a crash.
    static_cast<void>(injector.press(SystemButton::VolumeDown));
    injector.feed_input(pump.queue());
    pump.pump(root, loop);
    check(child->events.empty(), "an unfocused volume key is dropped");

    InputEvent tab;
    tab.type = InputEventType::KeyDown;
    tab.key = tessera::ui::key::kTab;
    pump.queue().push(tab);
    pump.pump(root, loop);
    child->events.clear();

    static_cast<void>(injector.press(SystemButton::VolumeUp));
    static_cast<void>(injector.press(SystemButton::Back));
    injector.feed_input(pump.queue());
    pump.pump(root, loop);
    check(child->events.size() == 1 && child->events[0].type == tessera::ui::EventType::VolumeKey &&
              child->events[0].volume_direction == VolumeDirection::Up,
          "the focused node receives the volume key and nothing else");
}

// --- Home, Resume and process death -------------------------------------------------------------

// A counter component: one int cell and one list cell, snapshot-able and restorable.
struct Counter {
    int count = 0;
    std::vector<std::string> items;

    [[nodiscard]] tessera::ui::StateSnapshot snapshot() const {
        tessera::ui::StateSnapshot cells;
        cells.push_back(tessera::ui::state_cell("state::count", count));
        cells.push_back(tessera::ui::state_cell("state::items", items));
        return cells;
    }
    static tessera::ui::StateSnapshot capture(void* user) {
        return static_cast<const Counter*>(user)->snapshot();
    }
};

AppState fold_state(const std::vector<OsEventType>& events, tessera::appfw::WindowLifecycle& window,
                    bool& suspended) {
    for (const OsEventType type : events) {
        LifecycleSignal signal{};
        if (!tessera::appfw::lifecycle_signal_for(type, signal)) {
            continue;
        }
        window = tessera::appfw::fold_window_lifecycle(window, signal);
        if (signal == LifecycleSignal::Suspend) {
            suspended = true;
        } else if (signal == LifecycleSignal::Resume) {
            suspended = false;
        }
    }
    return tessera::appfw::derive_app_state(window, suspended);
}

void home_then_resume_keeps_state_without_a_restore() {
    Counter counter;
    counter.count = 7;
    counter.items = {"a", "b"};

    SavedStateRecord record;
    SavedStateRecorder recorder(record);
    check(recorder.add_source("12345", &Counter::capture, &counter), "a source registers");

    tessera::runtime::ManualClock clock;
    SyntheticEventInjector script = tessera::hal::make_home_resume_script();
    tessera::appfw::WindowLifecycle window;
    bool suspended = false;

    OsEvent event;
    std::vector<OsEventType> seen;
    std::vector<AppState> states;
    while (script.pop(event)) {
        clock.advance(0.001);
        recorder.on_event(event.type);
        seen.push_back(event.type);
        states.push_back(fold_state(seen, window, suspended));
        seen.clear();
    }
    check((states == std::vector<AppState>{AppState::Hidden, AppState::Suspended, AppState::Hidden,
                                           AppState::Active}),
          "Home hides then suspends; Resume leaves it hidden until WindowShown");
    check(recorder.captures() == 1, "Suspend captured once");
    check(counter.count == 7 && counter.items.size() == 2,
          "a hot resume touched nothing: the live state is still there");
}

void process_death_then_relaunch_restores_state() {
    Counter before;
    before.count = 41;
    before.items = {"x", "y", "z"};

    SavedStateRecord record;
    SavedStateRecorder recorder(record);
    static_cast<void>(recorder.add_source("12345", &Counter::capture, &before));

    // Home, then the process dies: the script ends at Suspend, which is where the capture ran.
    SyntheticEventInjector script = tessera::hal::make_process_death_script();
    OsEvent event;
    while (script.pop(event)) {
        recorder.on_event(event.type);
    }
    check(recorder.captures() == 1 && record.size() == 1, "the Suspend captured the cells");

    // The relaunch is a cold start: a fresh component at its initializers, then the P4 rule.
    Counter relaunched;
    tessera::ui::StateSnapshot cells = relaunched.snapshot();
    const auto report = record.restore("12345", cells);
    check(report.restored == 2 && report.missing == 0 && report.kind_mismatch == 0,
          "both cells matched by id, name and kind");
    check(std::get<int>(cells[0].value) == 41, "the int cell is back");
    check(cells[1].is_list && cells[1].items.size() == 3, "and so is the list");

    // The rule's negative cases: another component, a new cell, a changed kind.
    tessera::ui::StateSnapshot other = relaunched.snapshot();
    const auto other_report = record.restore("99999", other);
    check(other_report.restored == 0 && other_report.missing == 2 &&
              std::get<int>(other[0].value) == 0,
          "state is never handed to a different component");

    tessera::ui::StateSnapshot changed;
    changed.push_back(tessera::ui::state_cell("state::count", 1.5));
    changed.push_back(tessera::ui::state_cell("state::added", true));
    const auto changed_report = record.restore("12345", changed);
    check(changed_report.kind_mismatch == 1 && changed_report.missing == 1 &&
              changed_report.restored == 0 && std::get<double>(changed[0].value) == 1.5,
          "a changed kind or a new cell starts at its initializer");

    // No record at all (the app saved nothing): a cold start with initializers.
    const SavedStateRecord empty;
    tessera::ui::StateSnapshot cold = relaunched.snapshot();
    check(empty.restore("12345", cold).restored == 0 && std::get<int>(cold[0].value) == 0,
          "an empty record restores nothing");
}

void closing_the_app_discards_the_record() {
    Counter counter;
    counter.count = 3;
    SavedStateRecord record;
    SavedStateRecorder recorder(record);
    static_cast<void>(recorder.add_source("1", &Counter::capture, &counter));
    check(recorder.on_event(OsEventType::Suspend) && !record.empty(), "captured at Suspend");
    check(!recorder.on_event(OsEventType::ScreenOff), "other events never capture");
    // An unhandled Back closes the app (spec rule 1): the state is discarded.
    recorder.discard();
    check(record.empty(), "the discard empties the record");
    check(!recorder.add_source("", &Counter::capture, &counter) &&
              !recorder.add_source("2", nullptr, nullptr),
          "an empty id or a null source is refused");
}

} // namespace

int main() {
    suite = "system_buttons";
    back_pops_overlay_then_route_then_reports_unhandled();
    repeated_back_on_an_empty_stack_stays_unhandled();
    back_without_an_app_handler_is_overlay_or_unhandled();
    buttons_map_onto_existing_events();
    a_full_home_is_refused_whole();
    ordering_and_coalescing_follow_os_events_section_3();
    volume_reaches_the_focused_node_and_back_reaches_no_node();
    home_then_resume_keeps_state_without_a_restore();
    process_death_then_relaunch_restores_state();
    closing_the_app_discards_the_record();
    if (failures != 0 || nav_test::g_failures != 0) {
        std::cerr << "system_buttons: " << failures + nav_test::g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
