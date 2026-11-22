// SPDX-License-Identifier: MIT
//
// The #454 acceptance test, written *only* with `tessera_testing`: a tap increments, a failed
// network fake shows the error state, and a back event pops a route. Each case is the shape the
// guide
// (`docs/guides/testing-your-app.md`) walks through, so a reader can copy a case and change what it
// asserts.
//
// What is deliberately NOT here: a pixel assertion. `expect_text`/`find`/`snapshot` are structural,
// so this file runs on every lane with no GPU, no window and no font. A pixel claim belongs in
// `tests/golden/`, and a lavapipe golden that skipped would not prove anything anyway.

#include "CounterCard.hpp"
#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/net_client.hpp"
#include "tessera/appfw/net_fake.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/testing/expect.hpp"
#include "tessera/testing/test_app.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace appfw = tessera::appfw;
namespace testing = tessera::testing;
namespace tier0 = tessera::runtime;
namespace ui = tessera::ui;
// A short alias for the layer-0 app namespace: every case's fixture is also named `app`, so the
// namespace cannot be spelled `app` here without being shadowed.
namespace tessapp = tessera::app;

// The pinned ids of `tests/fixtures/testing/CounterCard.tsx`. `automationId` replaces the computed
// `uiId` (automation-ids-v1.md section 2), which is what makes a literal id readable here.
constexpr std::uint64_t kRoot = 1001;
constexpr std::uint64_t kValue = 1002;
constexpr std::uint64_t kLabel = 1003;
constexpr std::uint64_t kAdd = 1004;
constexpr std::uint64_t kName = 1005;
constexpr std::uint64_t kPositive = 1006;

// The counter app under test: the generated component built into the app's scene, with the host's
// update scheduler attached so `setCount` actually reaches `apply_state()` in the next frame. That
// attachment is the one piece of wiring a real app's mount does and a test must not forget: without
// it a setter marks the component dirty and nothing ever flushes it.
class CounterApp final : public testing::TestApp {
public:
    CounterApp() {
        host().set_update_scheduler(&scheduler_);
    }

    void build_scene() override {
        card_.build(root());
        card_.mount();
        card_.attach_scheduler(scheduler_);
    }

    [[nodiscard]] card::CounterCard& card() noexcept {
        return card_;
    }

private:
    ui::UpdateScheduler scheduler_;
    card::CounterCard card_;
};

// --- 1. A tap increments -------------------------------------------------------------------------
//
// The headline acceptance of the issue: one `tap`, no manual frame pumping, the new text visible.
void test_a_tap_increments() {
    testing::note_case("a_tap_increments");
    CounterApp app;
    app.build();

    // Unique ids: a duplicate would make `find` silently pick the first one, and every later
    // assertion would be about a different node than the test names.
    testing::expect_eq(app.count_nodes(kAdd), std::size_t{1}, "the Add button is unique");
    app.expect_text(kValue, "Count: 0", "initial count");

    testing::expect(app.tap(kAdd), "tapping the Add button found a target");
    app.expect_text(kValue, "Count: 1", "count after one tap");
    app.expect_text(kLabel, "added", "the handler also set the label");

    testing::expect(app.tap(kAdd), "the second tap found a target");
    app.expect_text(kValue, "Count: 2", "count after two taps");
}

// A tap on an id that is not in the tree reports false instead of asserting somewhere deeper: the
// locator's absence is the test's information, not a crash.
void test_a_tap_on_a_missing_target_reports_false() {
    testing::note_case("a_tap_on_a_missing_target_reports_false");
    CounterApp app;
    app.build();
    testing::expect(!app.tap(9999), "an unknown automation id is not tappable");
    app.expect_absent(9999);
}

// --- 2. A conditional child appears --------------------------------------------------------------
//
// `{count > 0 && <Text>Positive</Text>}` is the smallest interesting structural change: the node
// does not exist before the tap and does exist after it, which is what
// `expect_absent`/`expect_exists` are for.
void test_a_conditional_child_appears_after_the_first_tap() {
    testing::note_case("a_conditional_child_appears_after_the_first_tap");
    CounterApp app;
    app.build();

    app.expect_absent(kPositive, "before any tap the conditional child is absent");
    testing::expect(app.tap(kAdd), "tapped Add");
    app.expect_exists(kPositive, "after one tap the conditional child exists");
    app.expect_text(kPositive, "Positive", "the conditional child's text");
}

// --- 3. Typing reaches the app through the real focus + IME path
// -----------------------------------
//
// `type_text` focuses the field through the production `FocusManager` and delivers the commit as an
// `EditText` through the pump, so the generated `onChange` handler is the one a real keyboard
// drives.
void test_typing_reaches_the_on_change_handler() {
    testing::note_case("typing_reaches_the_on_change_handler");
    CounterApp app;
    app.build();

    testing::expect(app.type_text(kName, "Ada"), "typed into the text field");
    // The field's onChange writes the label, which is the assertion that proves the whole path ran:
    // focus, commit, generated thunk, state, apply_state.
    app.expect_text(kLabel, "Ada", "the onChange handler wrote the label");
}

// A node that cannot take focus is reported rather than force-focused, because a real tap could not
// have put the caret there either.
void test_typing_into_a_non_focusable_target_reports_false() {
    testing::note_case("typing_into_a_non_focusable_target_reports_false");
    CounterApp app;
    app.build();
    testing::expect(!app.type_text(kValue, "nope"), "a Text node is not focusable");
}

// --- 4. A failed network fake shows the error state
// ------------------------------------------------
//
// The acceptance criterion is "a failed network fake shows the error state". `FakeNet` ships with
// `tessera_appfw_net`; what `tessera_testing` adds is the stack it needs standing up already - the
// completion queue, the manual IO executor, the timer queue and the manual clock - so a service can
// be written and tested without wiring four seams by hand.
//
// The "error state" is the app's own: a service turns a `NetResult` error into a status line, which
// is the state a test asserts on instead of a spinner animation or a pixel.

// A service with one dependency and one job: fetch, then write what came back into a label.
class WeatherService final : public appfw::Service {
public:
    static constexpr appfw::ServiceId kServiceId = appfw::service_id("test.weather");

    WeatherService(appfw::NetClient& client, ui::Node& label)
        : client_(client),
          label_(label) {}

    // The free-body form ADR-050 section 4 requires: a member coroutine cannot carry the
    // `TaskContext` first argument, so the override returns a context-first free function's
    // coroutine.
    tier0::Task<appfw::ServiceStatus> start(appfw::StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    tier0::Task<void> stop(appfw::StopContext& ctx) override {
        return run_stop(ctx, *this);
    }

    static tier0::Task<appfw::ServiceStatus> run_start(appfw::StartContext ctx,
                                                       WeatherService& self) {
        appfw::NetRequest request;
        request.url = "https://api.example.test/weather";
        appfw::NetResult result = co_await self.client_.send(std::move(request), ctx.task.token);
        write_status(self.label_, result);
        co_return appfw::ServiceStatus::Ok;
    }

    static tier0::Task<void> run_stop(appfw::StopContext /*ctx*/, WeatherService& /*self*/) {
        co_return;
    }

    // What the app does with the answer. The failure branch names the exact `NetErrorKind`, so an
    // assertion on it distinguishes a DNS failure from a connect or TLS one.
    static void write_status(ui::Node& label, const appfw::NetResult& result) {
        ui::PaintData data;
        data.kind = ui::PaintKind::Text;
        if (result.has_value()) {
            data.text = "weather: " + std::to_string(result->status);
        } else {
            data.text = "error: " + std::string(appfw::net_error_kind_name(result.error().kind));
        }
        label.set_paint_data(std::move(data));
        label.mark_dirty(ui::Dirty::Paint);
    }

private:
    appfw::NetClient& client_;
    ui::Node& label_;
};

void test_a_failed_network_fake_shows_the_error_state() {
    testing::note_case("a_failed_network_fake_shows_the_error_state");
    CounterApp app;
    app.build();

    // The status line is its own node: the acceptance is about the failure reaching the scene, not
    // about the counter component owning the label.
    auto status = std::make_unique<ui::Node>(9);
    status->set_element_kind("Text");
    status->set_automation_id(9001);
    ui::Node& status_ref = *status;
    app.root().add_child(std::move(status));

    appfw::FakeNet net(app.completions(), app.clock());
    // The scripted failure: a DNS error, which the spec distinguishes from `connect` and `tls`, so
    // the assertion below names the exact kind rather than "it failed".
    net.push_error(appfw::NetErrorKind::Dns, "no such host");

    appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                            app.timers(), app.clock());
    WeatherService weather(client, status_ref);

    // A concrete service registers as itself: `provide<T>` takes `Impl` derived from `T`, and the
    // service declares its own `kServiceId`. The expected is checked, not asserted through a cast,
    // so a registration failure reports which error it was.
    const auto registered = app.provide<WeatherService>(weather);
    testing::expect(registered.has_value(), "the weather service registers");
    if (!registered.has_value()) {
        std::cerr << "  registration failed: "
                  << appfw::service_error_code_name(registered.error().code) << "\n";
        return;
    }
    testing::expect(app.prepare().has_value(), "the service runner prepares");
    testing::expect(app.runner()->begin_start(), "the start sequence begins");

    // The fake delivers on the clock, so the test drives the exchange instead of waiting for it:
    // run the service up to its suspension, mark the scripted exchange due, then let the completion
    // drain.
    for (int attempt = 0; attempt < 8 && app.text_of(9001) == std::nullopt; ++attempt) {
        (void)app.executor().run(4);
        (void)app.executor().run_completions(4);
        net.deliver_due();
        app.advance(1.0 / 60.0);
        (void)app.completions().run(16);
        app.pump_ticks(1);
    }

    app.expect_text(9001, "error: dns", "the failed request reached the scene as an error");
}

// --- 5. Back pops a route
// --------------------------------------------------------------------------
//
// The third acceptance criterion. The route stack is `appfw::NavController`, and Back reaches it
// the way a device sends it: a `BackRequested` input event into the app's back handler (`AppHost`'s
// dispatch order is overlay, then handler, then unhandled).
void test_back_pops_a_route() {
    testing::note_case("back_pops_a_route");
    CounterApp app;
    app.build();

    // The route table the app ships. `RouteTable::create` validates it, so a test also covers the
    // declaration a real app would have failed to build.
    appfw::RouteTableConfig table_config;
    table_config.routes = {{.id = "home", .path = "/"}, {.id = "details", .path = "/details"}};
    table_config.not_found_route = "home";
    table_config.initial_route = "home";
    auto table = appfw::RouteTable::create(std::move(table_config));
    testing::expect(table.has_value(), "the route table validates");
    if (!table.has_value()) {
        return;
    }

    // The window's update scheduler is the one the route observable attaches its ticket to, so the
    // controller is built against the app's own.
    ui::UpdateScheduler scheduler;
    appfw::NavController nav(*table, scheduler);
    testing::expect(nav.push("/").has_value(), "pushed home");
    testing::expect(nav.push("/details").has_value(), "pushed details");
    nav.commit();
    testing::expect_eq(nav.depth(), std::size_t{2}, "two entries on the stack");

    // The app's Back handler: what `set_back_handler` installs in a real app. It consumes the
    // request by popping, and reports `false` at the root, which is the state a mobile host closes
    // the app on.
    struct BackContext {
        appfw::NavController* nav = nullptr;
    } context{&nav};

    app.host().set_back_handler(
        [](void* user) {
            auto* ctx = static_cast<BackContext*>(user);
            const appfw::BackResult result = ctx->nav->back();
            ctx->nav->commit();
            // Only an actual pop consumes the press. `AtRoot`/`Empty` fall through so the host
            // reports the press as unhandled, which is the documented exit signal.
            return result.outcome == appfw::BackOutcome::Popped ||
                   result.outcome == appfw::BackOutcome::DismissedModal;
        },
        &context);

    ui::InputEvent back;
    back.type = ui::InputEventType::BackRequested;
    app.send(back);

    testing::expect_eq(nav.depth(), std::size_t{1}, "Back popped one route");
    const appfw::NavEntry* active = nav.active();
    testing::expect(active != nullptr, "an entry is active after the pop");
    if (active != nullptr) {
        testing::expect_eq(active->route_id, std::string("home"), "the stack is back on home");
    }
    testing::expect(app.host().last_back_dispatch() == tessapp::BackDispatch::HandlerConsumed,
                    "the app's handler consumed the Back");

    // Back at the root: the handler reports it cannot consume, and the host records it as
    // unhandled, which is exactly what a mobile host closes the app on.
    app.send(back);
    testing::expect_eq(nav.depth(), std::size_t{1}, "Back at the root pops nothing");
    testing::expect(app.host().last_back_dispatch() == tessapp::BackDispatch::Unhandled,
                    "Back at the root is reported unhandled");
}

} // namespace

int main() {
    test_a_tap_increments();
    test_a_tap_on_a_missing_target_reports_false();
    test_a_conditional_child_appears_after_the_first_tap();
    test_typing_reaches_the_on_change_handler();
    test_typing_into_a_non_focusable_target_reports_false();
    test_a_failed_network_fake_shows_the_error_state();
    test_back_pops_a_route();

    if (testing::failures() != 0) {
        std::cerr << "testing_acceptance: " << testing::failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "testing_acceptance: ok\n";
    return 0;
}
