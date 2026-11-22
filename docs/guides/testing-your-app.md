# Testing your app

Status: `tessera_testing` is the app-level counterpart of the seams that already existed one layer
down. It is a target an app's *test* links, never the app (issue #454). Run the tests guide first for
the presets and the failure triage in [`testing.md`](testing.md).

An app is not the engine. The engine's own tests drive `AppHost`, the scene and the renderer with
hand-made fixtures; your app has a composition root, services, a store and a network client, and until
this target existed you had to stand all of that up yourself before asserting anything. `tessera_testing`
stands it up: one deterministic stack, the fakes that ship with the services they fake, and the verbs a
test actually wants (`tap`, `type_text`, `advance`, `find`, `expect_text`).

## 1. The shape of a test

A test is a `main()` returning nonzero, as everywhere else in the repo. There is deliberately no
framework: [`design/testing.md`](../design/testing.md) section 12 records that the decision to adopt one
has not been made, and a testing target is the wrong place to make it.

```cpp
#include "CounterCard.hpp"
#include "tessera/testing/expect.hpp"
#include "tessera/testing/test_app.hpp"

namespace testing = tessera::testing;
namespace ui = tessera::ui;

// Your app: the generated component plus the wiring a real mount does.
class CounterApp final : public testing::TestApp {
public:
    CounterApp() {
        // Without this a setter marks the component dirty and nothing ever flushes it. It is the one
        // piece of wiring a test reproduces from the app's own mount.
        host().set_update_scheduler(&scheduler_);
    }

    void build_scene() override {
        card_.build(root());
        card_.mount();
        card_.attach_scheduler(scheduler_);
    }

private:
    ui::UpdateScheduler scheduler_;
    card::CounterCard card_;
};

int main() {
    CounterApp app;
    app.build();                      // build_scene(), then one settled frame

    testing::expect_text(app, 1002, "Count: 0", "initial count");
    testing::expect(app.tap(1004), "tapped Add");
    testing::expect_text(app, 1002, "Count: 1", "count after one tap");

    return testing::failures();  // 0 when nothing failed
}
```

`expect_text(app, ...)` and friends are the free-function form; `app.expect_text(...)` is the same
assertion with the app as the receiver. Both are in the acceptance test.

## 2. Where the ids come from

`tap`/`find`/`expect_text` address **automation ids**, not coordinates. Two ways to get one:

- **Pin it.** `automationId="1004"` in the TSX. The generator replaces the computed `uiId` with your
  value, so the id is readable in the test and stable when the tree moves — which is what
  [`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 2 recommends for a long-lived target.
- **Read it from the manifest.** `generated/automation.manifest.json` lists every node's `uiId` against
  its source `line`/`column`, so a computed id is discoverable. That is slower to read but needs no TSX
  change.

An explicit `automationId` must be the node's `uiId`, and a numeric one *becomes* it — a hand-picked
value that contradicts the computed one is a build error (`TESSERA_IR_AUTOMATION_ID`), not a silent
override.

`count_nodes(id)` is the check that your locator is not ambiguous: a count above one means `find`
silently picked the first match and every later assertion is about a node you did not name.

## 3. The stack you get

`TestApp` owns every piece in the construction order that makes it valid — clock and queues before the
loop, loop before the host, host before the pump:

| Accessor | What it is |
|---|---|
| `clock()` | `ManualClock`. Starts at zero; only the test moves it. |
| `completions()` | `CompletionQueue`, drained by the loop each tick. |
| `executor()` | `ManualIoExecutor`: run the I/O body, then its completion, as separate steps. |
| `timers()` | `TimerQueue`, attached to the loop, so a deadline is a wake and not a sleep. |
| `loop()` | `RuntimeLoop` with the completion queue, the timer queue and render-on-demand. |
| `host()` | `AppHost` over `root()`. |
| `pump()` | `InputPump`, with the host installed as its input filter (see section 6). |
| `root()` | The scene root your component builds into. |
| `files()` | `appfw::FakeFileSystem`, the AF7 in-memory store. |
| `os_events()` | `hal::SyntheticEventInjector` for display/DPI/button scripts. |
| `services()` / `runner()` | The frozen scope and the service runner, after `prepare()`. |

Network and shell fakes are **not** duplicated here: `appfw::FakeNet` and `appfw::FakeShell` ship with
their modules and need the completion queue and the clock, so you construct one from `completions()` and
`clock()` and register it. Section 5 is the worked example.

## 4. Driving time

```cpp
app.advance(0.1);       // cover 100 ms in whole fixed ticks, painting when a frame is due
app.pump_ticks(6);      // exactly six iterations
app.pump_once();        // one iteration; true when it painted
```

A tick is `1/60 s` by default (`AdvanceConfig::tick_seconds`). `advance` rounds to a whole number of
ticks, so `advance(0.1)` and two `advance(0.05)` calls run the same number of frames and the same
animation samples — a property the kit's own suite asserts, because a test that depended on the
difference would be flaky for a reason nobody could see.

`AdvanceConfig::pump_io` (default on) runs one I/O body and its completion per tick. Turn it off to
assert that work is *still in flight*; that is how "the spinner is up" becomes a fact instead of a race.

`frames_painted()` and `ticks_run()` are counters, so "a static app idles" is asserted by a number, not
by waiting.

## 5. A failed network request

The acceptance criterion in issue #454, and the shape most app tests want: a service, a scripted failure,
a state the test can see.

```cpp
appfw::FakeNet net(app.completions(), app.clock());
net.push_error(appfw::NetErrorKind::Dns, "no such host");

appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                        app.timers(), app.clock());
WeatherService weather(client, status_node);

const auto registered = app.provide<WeatherService>(weather);   // scope + runner, in one call
if (!registered) { /* report registered.error().code */ }
app.prepare();
app.runner()->begin_start();

// The fake delivers on the clock, so you drive the exchange instead of waiting for it.
for (int attempt = 0; attempt < 8 && app.text_of(9001) == std::nullopt; ++attempt) {
    (void)app.executor().run(4);          // the I/O body
    (void)app.executor().run_completions(4);  // its UI-thread half
    net.deliver_due();                     // the scripted answer becomes due
    app.advance(1.0 / 60.0);               // the loop drains the completion queue
    (void)app.completions().run(16);
}

testing::expect_text(app, 9001, "error: dns", "the failure reached the scene");
```

A service must be written in the context-first free-body shape (ADR-050 section 4): a member coroutine
cannot carry `TaskContext` as its first argument. `tests/unit/appfw_test_services.hpp` has the working
form, and the acceptance test copies it.

`provide<T>(impl)` registers the implementation in the `ServiceContext` **and** adds it to the runner.
The real API keeps those separate because a service can be resolvable without being lifecycle-managed;
here the common case is both, and getting them out of step is exactly the mistake a test-support target
exists to prevent.

## 6. Back, Escape and the overlays

`TestApp` installs the host as the pump's input filter, as the window driver does. That is what routes
`BackRequested` to `AppHost::handle_overlay_input` and on to your handler — without it a Back would reach
the scene, which has no Back event, and your handler would never run.

```cpp
app.host().set_back_handler(
    [](void* user) {
        auto* nav = static_cast<appfw::NavController*>(user);
        const appfw::BackResult result = nav->back();
        nav->commit();                                      // frame-boundary publish
        return result.outcome == appfw::BackOutcome::Popped ||
               result.outcome == appfw::BackOutcome::DismissedModal;
    },
    &nav);

ui::InputEvent back;
back.type = ui::InputEventType::BackRequested;
app.send(back);

testing::expect(app.host().last_back_dispatch() == tessera::app::BackDispatch::HandlerConsumed,
                "the handler consumed the Back");
```

Returning `false` at the root is what makes the press report `Unhandled` — the documented exit signal a
mobile host acts on and a desktop host ignores.

## 7. OS events

```cpp
app.os_events().press(hal::SystemButton::Home);   // -> WindowHidden, Suspend; then pumped
app.os_events().press(hal::SystemButton::Back);   // -> BackRequested, through the pump
app.press(hal::SystemButton::Power);              // the same, plus the OS events applied to the host
```

`press` drains the OS events the app can observe (geometry, DPI, safe area) into the host and feeds the
input events into the pump. Named scripts — `make_home_resume_script()`, `make_process_death_script()`,
`make_back_pop_back_exit_script()` — are in [`os-events-v1.md`](../specs/os-events-v1.md) section 12 and
return a loaded injector you drive the same way.

## 8. Structural, not pixel

`snapshot()` returns the canonical automation document for the live tree. Assert on it when a claim is
about structure — node order, accessible names, item keys — rather than text at one node:

```cpp
const auto document = app.snapshot();
testing::expect(document.has_value(), "the tree is within the snapshot caps");
```

A pixel claim belongs in a golden (`tests/golden/`), and a lavapipe golden that **skips** is not a pass.
Keeping app tests structural is what lets the same binary run on Windows, Linux, lavapipe and the Wasm
lane with no GPU, no window and no font.

## 9. Determinism

Everything is an argument: the clock, the I/O executor, the completion queue, the timer queue, the
network scripts. Nothing sleeps, reads a wall clock, opens a socket, starts a thread or touches a real
file. Two runs of the same test produce byte-identical output — which is what makes a counter assertion
worth writing.

What that does **not** give you: a real driver. `TestApp` has no window, no swapchain and no platform
event loop, so it cannot catch a bug that only happens in `WindowHost`. Those live in
`tests/gpu/app_window_*` and need the real-GPU lane ([`real-gpu-ci.md`](real-gpu-ci.md)).

## 10. What the target may depend on

`tessera_testing` is a static library, and no shipping target links it. Its test
(`tests/unit/testing/kit_tests.cpp`) asserts the kit's guarantees; the build graph asserts the
separation. If you add a dependency to it, check it is not a backend: a window or GPU backend here would
make every app test require a display, which is the failure mode the whole test-only split exists to
avoid.

Implementation: `src/testing/` (`tessera_testing`), the fakes under `include/tessera/appfw/`
(`filesystem_fake.hpp`, `net_fake.hpp`, `shell_fake.hpp`), the injector in
`include/tessera/hal/synthetic_events.hpp`, the assertion helpers in `include/tessera/testing/expect.hpp`.
The worked example is `tests/unit/testing/acceptance_tests.cpp`; the fixture component it builds is
`tests/fixtures/testing/CounterCard.tsx`.
