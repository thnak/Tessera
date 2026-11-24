// SPDX-License-Identifier: MIT
//
// The #454 acceptance test, written *only* with `tessera_testing`: a tap increments, a failed
// network fake shows the error state, and a back event pops a route. Each case is the shape the
// guide (`docs/guides/testing-your-app.md`) walks through, so a reader can copy a case and change
// what it asserts. The #838 helpers (`mount`, `settle`, `run_until`) are the default form; one
// case keeps the old hand-written pump loop so both paths stay covered.
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
constexpr std::uint64_t kValue = 1002;
constexpr std::uint64_t kLabel = 1003;
constexpr std::uint64_t kAdd = 1004;
constexpr std::uint64_t kName = 1005;
constexpr std::uint64_t kPositive = 1006;
// The status line the network cases write into: its own node, because the acceptance is about the
// failure reaching the scene, not about the counter component owning the label.
constexpr std::uint64_t kStatus = 9001;

// The app under test. `build_scene` is one `mount` call: the generated component's
// `build(root())` + `mount()` + `attach_scheduler(scheduler())` in the order a real app's mount
// does them. The scheduler itself is `TestApp`'s and is already installed on the host, so there is
// no wiring left to forget - before #838 both halves were hand-written and forgetting either left
// a setter marking a ticket nothing ever flushed.
class ServicesApp final : public testing::TestApp {
public:
    void build_scene() override {
        mount(card_);
        auto status = std::make_unique<ui::Node>(9);
        status->set_element_kind("Text");
        status->set_automation_id(kStatus);
        status_ = status.get();
        root().add_child(std::move(status));
    }

    [[nodiscard]] card::CounterCard& card() noexcept {
        return card_;
    }

    [[nodiscard]] ui::Node& status() noexcept {
        return *status_;
    }

private:
    card::CounterCard card_;
    ui::Node* status_ = nullptr;
};

// --- 1. A tap increments -------------------------------------------------------------------------
//
// The headline acceptance of the issue: one `tap`, no manual frame pumping, the new text visible.
void test_a_tap_increments() {
    testing::note_case("a_tap_increments");
    ServicesApp app;
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
    ServicesApp app;
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
    ServicesApp app;
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
    ServicesApp app;
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
    ServicesApp app;
    app.build();
    testing::expect(!app.type_text(kValue, "nope"), "a Text node is not focusable");
}

// --- 4. A failed network fake shows the error state
// ------------------------------------------------
//
// The acceptance criterion is "a failed network fake shows the error state". `FakeNet` ships with
// `tessera_appfw_net`; what `tessera_testing` adds is the stack it needs standing up already - the
// completion queue, the manual IO executor, the timer queue and the manual clock - and, since
// #838, the delivery: `attach(net)` is what lets `settle` hand the fake's due answer to the
// completion queue, so the test never names the fake in a pump loop.
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

// Shared start-up of the network cases: the caller has built the app (so the status node exists)
// and owns `net`, `client` and the service - the runner borrows them, so they must outlive the
// app's run, exactly as in the original long form.
void register_and_start(ServicesApp& app, WeatherService& weather) {
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
}

void test_a_failed_network_fake_shows_the_error_state() {
    testing::note_case("a_failed_network_fake_shows_the_error_state");
    ServicesApp app;
    app.build();

    appfw::FakeNet net(app.completions(), app.clock());
    // The scripted failure: a DNS error, which the spec distinguishes from `connect` and `tls`, so
    // the assertion below names the exact kind rather than "it failed".
    net.push_error(appfw::NetErrorKind::Dns, "no such host");
    app.attach(net);
    appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                            app.timers(), app.clock());
    WeatherService weather(client, app.status());
    register_and_start(app, weather);

    // The exchange the guide's section 5 used to drive by hand: `settle` runs the same steps in
    // the same order until nothing is pending. The bound is what turns "the fake never answered"
    // into a named failure instead of a test that passes on a timeout.
    const testing::SettleResult settled = app.settle();
    testing::expect(settled.outcome == testing::SettleOutcome::Settled,
                    "the exchange settled before the bound");

    app.expect_text(kStatus, "error: dns", "the failed request reached the scene as an error");
}

// The "until node 9001 has text" form: the answer is delayed 120 ms on the fake's clock, and the
// test expresses *when* instead of hand-counting ticks.
void test_run_until_waits_for_the_node_to_change() {
    testing::note_case("run_until_waits_for_the_node_to_change");
    ServicesApp app;
    app.build();

    appfw::FakeNet net(app.completions(), app.clock());
    appfw::NetResponse response;
    response.status = 200;
    response.body = "ok";
    net.push_response(response, 120);
    app.attach(net);
    appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                            app.timers(), app.clock());
    WeatherService weather(client, app.status());
    register_and_start(app, weather);

    const testing::SettleResult ran =
        app.run_until([&app] { return app.text_of(kStatus).has_value(); });
    testing::expect(ran.outcome == testing::SettleOutcome::Settled,
                    "the delayed response arrived before the bound");
    testing::expect(ran.ticks > 1, "the delay cost more than one step");
    app.expect_text(kStatus, "weather: 200", "the success reached the scene");
}

// A deliberately unsettleable exchange: the scripted answer is due an hour out, far past the
// bound, so the run stops at the bound and says so. This is the report a test sees when its
// service never answers - `BoundReached` with the counts, not a silent pass.
void test_an_unanswerable_exchange_reports_the_bound() {
    testing::note_case("an_unanswerable_exchange_reports_the_bound");
    ServicesApp app;
    app.build();

    appfw::FakeNet net(app.completions(), app.clock());
    net.push_error(appfw::NetErrorKind::Dns, "no such host", 3'600'000);
    app.attach(net);
    appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                            app.timers(), app.clock());
    WeatherService weather(client, app.status());
    register_and_start(app, weather);

    testing::SettleConfig config;
    config.max_ticks = 8;
    const testing::SettleResult ran = app.settle(config);
    testing::expect(ran.outcome == testing::SettleOutcome::BoundReached,
                    "the far-out answer is reported as the bound, not as settled");
    testing::expect_eq(ran.ticks, std::size_t{8}, "the bound, not convergence, stopped the run");
    testing::expect(app.text_of(kStatus) == std::nullopt,
                    "the answer never reached the scene within the bound");
}

// --- 5. The old long form ------------------------------------------------------------------------
//
// The same exchange driven step by step, exactly as the guide asked for it before #838: run the
// I/O body, run its UI-thread half, deliver what the fake owes, advance, drain the completion
// queue. The order matters and every test that touched a service used to copy it; the case stays
// so the explicit step API underneath `settle` keeps working and stays covered.
void test_a_failed_network_fake_shows_the_error_state_long_form() {
    testing::note_case("a_failed_network_fake_shows_the_error_state_long_form");
    ServicesApp app;
    app.build();

    appfw::FakeNet net(app.completions(), app.clock());
    net.push_error(appfw::NetErrorKind::Dns, "no such host");
    appfw::NetClient client(appfw::NetClientConfig{}, net, app.executor(), app.completions(),
                            app.timers(), app.clock());
    WeatherService weather(client, app.status());
    register_and_start(app, weather);

    // The fake delivers on the clock, so the test drives the exchange instead of waiting for it:
    // run the service up to its suspension, mark the scripted exchange due, then let the completion
    // drain.
    for (int attempt = 0; attempt < 8 && app.text_of(kStatus) == std::nullopt; ++attempt) {
        (void)app.executor().run(4);
        (void)app.executor().run_completions(4);
        net.deliver_due();
        app.advance(1.0 / 60.0);
        (void)app.completions().run(16);
        app.pump_ticks(1);
    }

    app.expect_text(kStatus, "error: dns", "the failed request reached the scene as an error");
}

// --- 6. Back pops a route
// --------------------------------------------------------------------------
//
// The third acceptance criterion. The route stack is `appfw::NavController`, and Back reaches it
// the way a device sends it: a `BackRequested` input event into the app's back handler (`AppHost`'s
// dispatch order is overlay, then handler, then unhandled). The controller attaches its update
// ticket to the kit's own scheduler (`app.scheduler()`), which is the same one the host flushes.
void test_back_pops_a_route() {
    testing::note_case("back_pops_a_route");
    ServicesApp app;
    app.build();

    // The route table the app ships. `RouteTable::create` validates it, so a test also covers the
    // declaration a real app would have failed to build. Built field by field rather than with a
    // braced initializer list: a designated-initializer list that omits `params` trips
    // `-Wmissing-field-initializers`, which is an error under this project's `-Werror` (MSVC does
    // not warn, so only the Linux lane catches it).
    appfw::RouteTableConfig table_config;
    appfw::RouteDef home;
    home.id = "home";
    home.path = "/";
    appfw::RouteDef details;
    details.id = "details";
    details.path = "/details";
    table_config.routes = {home, details};
    table_config.not_found_route = "home";
    table_config.initial_route = "home";
    auto table = appfw::RouteTable::create(std::move(table_config));
    testing::expect(table.has_value(), "the route table validates");
    if (!table.has_value()) {
        return;
    }

    appfw::NavController nav(*table, app.scheduler());
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
    test_run_until_waits_for_the_node_to_change();
    test_an_unanswerable_exchange_reports_the_bound();
    test_a_failed_network_fake_shows_the_error_state_long_form();
    test_back_pops_a_route();

    if (testing::failures() != 0) {
        std::cerr << "testing_acceptance: " << testing::failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "testing_acceptance: ok\n";
    return 0;
}
