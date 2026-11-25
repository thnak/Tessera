// SPDX-License-Identifier: MIT
//
// `tessera_testing`: app-level test support (issue #454, ADR-050). It is the app-level counterpart
// of the seams that already existed one layer down: `ManualClock`,
// `ManualExecutor`/`ManualIoExecutor`, `FakeNet`, `FakeShell`, `SyntheticEventInjector`, the
// headless `AppHost` and the automation snapshot. What was missing was a way to stand all of them
// up together and drive an app the way the engine's own tests are driven.
//
// **Where it lives and what it links.** The headers are under `include/tessera/testing/` and the
// implementation is the `tessera_testing` target, which no shipping target links: an app's test
// binary links it, the app does not. `docs/guides/testing-your-app.md` is the worked example.
//
// **Settling (issue #838).** Two pieces of wiring every app test repeated by hand now live here:
// the `UpdateScheduler` (owned, installed on the host, attached by `mount(Component&)`) and the
// pump loop a network or shell exchange needs (`settle()` / `run_until()`). Both are built on the
// explicit step API below and hide nothing a test may need directly.
//
// **What it deliberately does not do.** It does not add a test framework. The repo has none on
// purpose (`docs/design/testing.md` section 12: a `main()` returning nonzero and a clear message),
// and a second convention here would be a second thing to keep working. `expect.hpp` is the
// assertion vocabulary of that existing style, not a runner.
//
// **Determinism, stated precisely.** Every instant comes from `TestApp::clock()`, every I/O body
// runs on `TestApp::executor()`, every completion on `TestApp::completions()`, every timer on
// `TestApp::timers()`, and the network and shell fakes answer from a script. `advance()` moves the
// clock and pumps; nothing here sleeps, reads a wall clock, opens a socket, starts a thread or
// touches a real file. Two runs of the same test produce the same frames and the same output.

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/appfw/filesystem_fake.hpp"
#include "tessera/appfw/service_context.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/hal/synthetic_events.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/timer_queue.hpp"
#include "tessera/testing/expect.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations, not includes: the fakes' own headers name the modules they fake, and a
// test that uses one includes that header anyway (it has to, to construct the fake). The kit only
// borrows a pointer to call `deliver_due()`.
namespace tessera::appfw {
class FakeNet;
class FakeShell;
} // namespace tessera::appfw

namespace tessera::testing {

// How a scripted `advance()` moves time. The frame loop is fixed-step, so a span of simulated time
// is a whole number of ticks; the two knobs below decide how they are spaced.
struct AdvanceConfig {
    // Simulated seconds per tick. 1/60 matches the engine's own event pacing, so a script's timing
    // is the timing a real app sees.
    double tick_seconds = 1.0 / 60.0;
    // Pumps the IO executor (`run_one` + its completion) before each tick. Off for a test that
    // wants to observe "the work is still in flight"; on for one that wants it to finish.
    bool pump_io = true;
};

// What one deterministic step of `settle` / `run_until` covers. The fixed order below is the one
// the guide's hand-written pump loop copied; putting it in one place is what keeps every test from
// re-deriving it (and from getting it wrong, where a wrong order shows up as a one-tick lag or a
// completion that never drains). The field initializers keep it valid with no explicit assignment.
struct SettleConfig {
    // Simulated seconds per step. 1/60 matches the engine's own event pacing.
    double tick_seconds = 1.0 / 60.0;
    // The I/O budget of one step, both queues of `ManualIoExecutor`.
    std::size_t io_budget = 16;
    // The completion-queue drain budget of one step.
    std::size_t completion_budget = 16;
    // The upper bound on steps. `settle` returns `BoundReached` at it rather than looping forever,
    // so a test whose exchange never resolves fails loudly at a named bound instead of hanging.
    std::size_t max_ticks = 64;
    // Pumps the IO executor and delivers the attached fakes before each step. Off freezes the
    // worker half and the fakes - the queued work is the "still in flight" state the test asked to
    // observe - while the completion queue and the loop keep draining, so a settle ends at the
    // first step instead of reporting the frozen work as an unsettled exchange.
    bool pump_io = true;
};

// Which bound a `settle` hit, and the counts that say what the bound cost.
enum class SettleOutcome : std::uint8_t {
    // Nothing was pending (executor, completion queue and timers) on the last step.
    Settled,
    // The configured bound was hit first. Never a silent pass: a caller that requires a settled
    // exchange asserts on the outcome, so a service that never answers is a named failure.
    BoundReached,
};

struct SettleResult {
    SettleOutcome outcome = SettleOutcome::Settled;
    // Steps run, and the totals each step reported, so a test can bound a claim ("it took one
    // step") instead of only a pass/fail.
    std::size_t ticks = 0;
    std::size_t completions = 0;
    std::size_t timers_fired = 0;
    std::size_t io_ran = 0;
};

// One app under test: the deterministic stack, the scene, and the driving verbs.
//
// The construction order matters and is fixed here rather than left to each call site: the clock
// and the queues have to exist before the loop, the loop before the host, and the host before the
// pump can dispatch into it. Getting that order wrong is a use-after-free that a test would report
// as a mysterious crash, so `TestApp` owns every piece in one place.
//
// The root node is a member, so a component built into it must be built by the test (a generated
// component holds `Node*`s into the tree it built). `build()` is that hook: it runs at
// construction, after the host exists, so a component sees a live host and a laid-out frame.
class TestApp {
public:
    struct Config {
        // The window the app is tested at, in logical pixels. The default is a phone in portrait,
        // so a layout-sensitive assertion is written against a realistic box.
        float width = 390.0f;
        float height = 844.0f;
        float dpi_scale = 1.0f;
        // The stylesheet the host resolves against. `ui::style::compile_stylesheet("")` is a valid
        // empty sheet, which is what a component with only inline styles needs.
        std::string stylesheet;
        AdvanceConfig advance{};
    };

    // Builds the stack, lays out one frame, and runs `Config`'s stylesheet. The component's build
    // hook is not run yet: call `build()` once.
    //
    // Two constructors instead of `Config = {}`: a default argument would have to name the nested
    // `AdvanceConfig`'s default member initializer while `TestApp` is still incomplete, which GCC
    // rejects (MSVC accepts it, so only a second compiler catches it). The same workaround and the
    // same reason as `EngineAutomationHost` in `include/tessera/automation/engine_host.hpp`.
    TestApp();
    explicit TestApp(Config config);
    ~TestApp();

    TestApp(const TestApp&) = delete;
    TestApp& operator=(const TestApp&) = delete;

    // --- composition root -------------------------------------------------------------------

    // Registers a service implementation under `T::kServiceId` (ADR-050 section 3). Call before
    // `prepare()`. A duplicate id or a full context is reported by the returned error, exactly as
    // the real composition root would see it, so a test also covers the wiring. Registers a
    // service: it becomes resolvable through the `ServiceContext` *and* it joins the runner, which
    // is what a real composition root does with two calls (`provide` on the scope, `add` on the
    // runner). Call before `prepare()`.
    //
    // The two are separate in the real API because a service can be resolvable without being
    // lifecycle-managed (a value the app only reads). Here the common case is both, and getting
    // them out of step is the mistake a test-support target exists to prevent: a service that is
    // resolvable but never started would leave its `start()` un-run and the test waiting on work
    // that never begins.
    template<appfw::ProvidedService T, std::derived_from<T> Impl>
    std::expected<void, appfw::ServiceError> provide(Impl& implementation, bool required = false) {
        if (prepared_) {
            return std::unexpected(
                appfw::ServiceError{appfw::ServiceErrorCode::Frozen, T::kServiceId, {}});
        }
        if (const auto provided = services_.provide<T>(implementation); !provided) {
            return provided;
        }
        // The runner needs the concrete `Service`. A registered type that is not one (a plain
        // value) is legal for the scope but cannot be lifecycle-managed, which is a compile-time
        // fact here rather than a runtime surprise.
        if constexpr (std::derived_from<Impl, appfw::Service>) {
            appfw::ServiceRegistration registration;
            registration.id = T::kServiceId;
            registration.required = required;
            if (const auto added = runner_.add(registration, implementation); !added) {
                return added;
            }
        } else {
            static_assert(!std::derived_from<T, appfw::Service>,
                          "a Service must be registered as the concrete service type, because the "
                          "runner takes a Service& and provide<T> would slice the identity");
        }
        return {};
    }

    // Freezes the service registry and prepares the runner. Idempotent; returns the first error.
    std::expected<void, appfw::ServiceError> prepare();

    // The hook a test overrides to build its component into `root()`. Runs after the host exists.
    // Default does nothing, which is the "no app, just the stack" case.
    virtual void build_scene() {}

    // Runs `build_scene()` (once), then one settled frame so bounds and hit testing are valid. Safe
    // to call once; a second call is a no-op, so a fixture's constructor can call it and a test
    // that forgot to can still call it.
    void build();

    // --- composition: mount and attach ------------------------------------------------------

    // The scheduler every generated component's update ticket attaches to, installed on the host.
    // `TestApp` owns it so a test cannot forget the wiring: a host without a scheduler silently
    // drops `setCount`-style updates, which a test would read as "the handler did not run".
    [[nodiscard]] ui::UpdateScheduler& scheduler() noexcept {
        return scheduler_;
    }

    // The one call a test's `build_scene()` needs: `component.build(root())`, `component.mount()`,
    // then `component.attach_scheduler(scheduler())`. The three together are what a real app's
    // mount does; forgetting the scheduler leaves a setter marking a ticket nothing ever flushes,
    // with no hint why - which is the mistake this helper exists to make unreachable. Call it from
    // `build_scene()`; `build()` runs `build_scene()` exactly once, so a component is never built
    // twice.
    template<typename Component> void mount(Component& component) {
        component.build(root());
        component.mount();
        component.attach_scheduler(scheduler());
    }

    // Registers a fake that delivers on the clock (`appfw::FakeNet`, `appfw::FakeShell`): settle
    // and the step API then call its `deliver_due()` every step, so the test never names the fake
    // in a pump loop. Borrowed; the fake must outlive the app. The fakes already need the app's
    // completion queue and clock as constructor arguments, so this only completes the borrowing.
    void attach(appfw::FakeNet& fake);
    void attach(appfw::FakeShell& fake);

    // --- the deterministic stack -------------------------------------------------------------

    [[nodiscard]] runtime::ManualClock& clock() noexcept {
        return clock_;
    }
    [[nodiscard]] runtime::CompletionQueue& completions() noexcept {
        return completions_;
    }
    [[nodiscard]] runtime::ManualIoExecutor& executor() noexcept {
        return executor_;
    }
    [[nodiscard]] runtime::TimerQueue& timers() noexcept {
        return timers_;
    }
    [[nodiscard]] runtime::RuntimeLoop& loop() noexcept {
        return loop_;
    }
    [[nodiscard]] app::AppHost& host() noexcept {
        return host_;
    }
    [[nodiscard]] ui::InputPump& pump() noexcept {
        return pump_;
    }
    [[nodiscard]] ui::Node& root() noexcept {
        return root_;
    }
    // The frozen service scope. Null until `prepare()` succeeded.
    [[nodiscard]] const appfw::ServiceContext* services() const noexcept {
        return prepared_ ? &services_ : nullptr;
    }
    [[nodiscard]] appfw::ServiceRunner* runner() noexcept {
        return prepared_ ? &runner_ : nullptr;
    }
    // The two fakes this target owns outright. `files()` is the AF7 in-memory file system that
    // ships with the storage module; `os_events()` is the synthetic injector of os-events-v1.md
    // sections 3 and 12. The network and shell fakes already ship with their modules and need
    // constructor arguments (the completion queue and the clock), so a test that wants one
    // constructs it from `completions()` and `clock()` and hands it to `attach(fake)`, which is
    // what lets `settle` deliver its due work; they are not duplicated here.
    [[nodiscard]] appfw::FakeFileSystem& files() noexcept {
        return files_;
    }
    [[nodiscard]] hal::SyntheticEventInjector& os_events() noexcept {
        return os_events_;
    }

    // --- time ---------------------------------------------------------------------------------

    // Simulated seconds since the app was created.
    [[nodiscard]] double now() const;

    // Moves simulated time forward by `duration` in fixed ticks, pumping the loop, the completion
    // queue, the IO executor and the timers on each one, and painting when the pipeline says a
    // frame is due. This is the one verb that makes "after 100 ms the transition is half done" a
    // fact.
    void advance(double duration, AdvanceConfig config = {});

    // Runs exactly `ticks` iterations of the same loop, for a test that counts frames rather than
    // time.
    void pump_ticks(std::size_t ticks, AdvanceConfig config = {});

    // One iteration: drain completions, fire due timers, tick the loop, run one frame, paint when
    // due. Returns whether a frame was painted.
    bool pump_once();

    // --- settling an exchange (issue #838) ---------------------------------------------------

    // Drives the app until nothing is pending or the bound is hit, one `SettleConfig` step at a
    // time. One step, in the fixed order: run I/O bodies and their completions, deliver what the
    // attached fakes owe at the current instant, drain the completion queue, tick the loop (which
    // fires due timers and drains completions again), then run one frame so the scene shows the
    // result. Nothing is pending means: the executor holds no work or completions, the completion
    // queue is empty, and every timer's deadline is after `now`.
    //
    // A body that posts new I/O still settles, because the step order re-runs it before the
    // pending check. What cannot settle - a service suspended on a fake that was never attached,
    // a scripted answer with a delay the test never advances past - reports `BoundReached` with
    // the counts, which turns "the test passed on a timeout" into a named, assertable failure.
    //
    // Delivering a due fake posts a completion and nothing else, so an empty final step with no
    // deliverable is the convergence signal; the loop is bounded by `max_ticks` regardless.
    SettleResult settle(const SettleConfig& config = {});

    // The `until node 9001 has text` form: one whole step per check, until `predicate` holds or
    // `max_ticks` steps have run. The result says which, with the totals, so "the bound was hit
    // before the node ever showed the text" is an assertion and not a hang. Steps run even when
    // nothing is pending, because a predicate may watch simulated time or a counter rather than a
    // queue; a predicate that never holds costs exactly `max_ticks` deterministic steps, never a
    // hang.
    template<typename Predicate>
    SettleResult run_until(Predicate&& predicate, const SettleConfig& config = {}) {
        SettleResult result;
        result.outcome = SettleOutcome::BoundReached;
        for (std::size_t tick = 0; tick < config.max_ticks; ++tick) {
            const SettleResult step = settle_step(config);
            result.ticks += step.ticks;
            result.completions += step.completions;
            result.timers_fired += step.timers_fired;
            result.io_ran += step.io_ran;
            if (static_cast<bool>(predicate())) {
                result.outcome = SettleOutcome::Settled;
                break;
            }
        }
        return result;
    }

    // One forced frame: the settle a test wants after it changed the tree or dispatched input,
    // where "was a frame due" is not the question being asked. `build()` runs one at construction.
    void settle_frame();

    // --- input --------------------------------------------------------------------------------

    // A full pointer gesture on the node with this automation id: move, down, up, then a settle.
    // The whole gesture goes through the pump's bounded queue and the production `InputRouter`, so
    // hover, implicit capture, focus and click synthesis are the real ones, not a mock of them.
    // `false` when no visible node has that id.
    bool tap(std::uint64_t automation_id);

    // Types `text` into the node with this automation id: focuses it, then delivers each chunk as
    // an `EditText` through the pump, exactly as an IME commit does. `false` when the node is
    // absent or not focusable.
    bool type_text(std::uint64_t automation_id, std::string_view text);

    // Dispatches one normalized event through the pump. Use it for the events `tap`/`type_text` do
    // not cover (a wheel, a key, `BackRequested`, a system button from `os_events()`).
    void send(const ui::InputEvent& event);
    // Delivers one system button press (the `hal::SyntheticEventInjector` mapping): its OS events
    // through `dispatch_os_events`, then its input events through the pump.
    void press(hal::SystemButton button);
    // Drains every queued OS event of `os_events()` into the host and the window registry, in
    // order.
    void dispatch_os_events();

    // --- the tree -----------------------------------------------------------------------------

    // The first node (preorder) with this automation id, skipping a subtree that is not rendered.
    // nullptr when absent, which is the answer a test asserts against.
    [[nodiscard]] ui::Node* find(std::uint64_t automation_id);
    [[nodiscard]] const ui::Node* find(std::uint64_t automation_id) const;

    // The text payload of that node's `PaintData`, or nullopt when the node is absent or paints no
    // text. `expect_text` is the assertion; this is the raw value.
    [[nodiscard]] std::optional<std::string> text_of(std::uint64_t automation_id) const;

    // The canonical automation snapshot of the live scene (docs/specs/automation-ids-v1.md section
    // 5.2), which is what a structural assertion compares instead of pixels. `std::nullopt` when
    // the tree exceeded the walk's caps.
    [[nodiscard]] std::optional<ui::automation::UiSnapshot>
    snapshot(bool include_hidden = false) const;

    // Counts nodes carrying `automation_id`: 1 is unique, more than 1 means the test's locator is
    // ambiguous and `find` silently picked the first one.
    [[nodiscard]] std::size_t count_nodes(std::uint64_t automation_id) const;

    // --- assertions ---------------------------------------------------------------------------

    // Asserts the node exists and paints exactly `expected`. Returns the comparison so a caller can
    // chain; a mismatch is recorded through `expect`/`EXPECT_*`, so the test's own failure count
    // decides the exit code.
    bool expect_text(std::uint64_t automation_id, std::string_view expected,
                     std::string_view what = "text") const;
    bool expect_exists(std::uint64_t automation_id, std::string_view what = "node") const;
    bool expect_absent(std::uint64_t automation_id, std::string_view what = "node") const;

    // How many frames the app painted since it was created. "A static app idles at zero presents"
    // is a counter assertion, not a timing one.
    [[nodiscard]] std::size_t frames_painted() const noexcept {
        return frames_painted_;
    }
    [[nodiscard]] std::size_t ticks_run() const noexcept {
        return ticks_run_;
    }

private:
    // The full single-iteration body. `pump_ticks` and `advance` both call it, so the two cannot
    // drift; they differ only in how many times and how far the clock moves first.
    void tick_once(const AdvanceConfig& config);
    void run_frame(bool force);
    void paint_settled_frame();
    // The one step of `settle`; returns its counts with `outcome` reporting whether anything was
    // pending when the step ended.
    SettleResult settle_step(const SettleConfig& config);
    // Delivers what every attached fake owes at the current instant; returns how many were
    // delivered.
    std::size_t deliver_fakes();
    void count_in(const ui::Node& node, std::uint64_t automation_id, std::size_t& count) const;

    Config config_;
    ui::Node root_{1};
    runtime::ManualClock clock_;
    runtime::CompletionQueue completions_;
    runtime::ManualIoExecutor executor_;
    runtime::TimerQueue timers_;
    runtime::RuntimeLoopConfig loop_config_;
    runtime::RuntimeLoop loop_;
    app::AppHost host_;
    ui::InputPump pump_;
    appfw::ServiceContext services_;
    appfw::ServiceRunner runner_;
    appfw::FakeFileSystem files_;
    hal::SyntheticEventInjector os_events_;
    // Owned and installed on the host at construction (issue #838): the wiring a fixture used to
    // repeat - `host().set_update_scheduler(&scheduler_)` - is done once, here, in the right order
    // relative to the host's reduced-motion preference.
    ui::UpdateScheduler scheduler_;
    // Borrowed clock-driven fakes (`attach`), so settle can deliver without the test naming them.
    std::vector<appfw::FakeNet*> fake_net_;
    std::vector<appfw::FakeShell*> fake_shell_;
    std::size_t frames_painted_ = 0;
    std::size_t ticks_run_ = 0;
    // The frame count at the start of the current `pump_once`, so the return value is "did THIS
    // iteration paint" rather than "has anything ever painted".
    std::size_t last_frames_seen_ = 0;
    bool built_ = false;
    bool prepared_ = false;
};

} // namespace tessera::testing
