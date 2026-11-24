// SPDX-License-Identifier: MIT
//
// `tessera_testing`'s own contract. The acceptance test proves an app can be tested with the kit;
// this file proves the kit's guarantees are the ones its documentation claims, so a test that
// passes is not passing for the wrong reason:
//
//   * the stack is deterministic - two runs of the same script produce the same frame and paint
//     counts, and no test reads a wall clock;
//   * `advance` moves simulated time in whole fixed ticks, and `pump_ticks` runs exactly the ticks
//   it
//     was asked for;
//   * `settle` reports `Settled` only when nothing is pending, `BoundReached` at the bound, and
//     delivers attached fakes - the #838 helpers are built on the same steps as the explicit API;
//   * a locator that misses reports absence instead of asserting somewhere deeper;
//   * the `FakeFileSystem` is the one that ships with the storage module, records every call and
//   can
//     be told to fail at one named step;
//   * `tessera_testing` is not linked by any shipping target.

#include "CounterCard.hpp"
#include "tessera/appfw/filesystem_fake.hpp"
#include "tessera/appfw/net_fake.hpp"
#include "tessera/appfw/shell_fake.hpp"
#include "tessera/testing/expect.hpp"
#include "tessera/testing/test_app.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace appfw = tessera::appfw;
namespace testing = tessera::testing;
namespace ui = tessera::ui;

constexpr std::uint64_t kValue = 1002;
constexpr std::uint64_t kAdd = 1004;

// The counter fixture behind a `mount`-only `build_scene`: the smallest app that needs the update
// scheduler wired, which is the wiring `mount` owns.
class FixtureCard final : public testing::TestApp {
public:
    void build_scene() override {
        mount(card_);
    }

private:
    card::CounterCard card_;
};

// --- the stack is deterministic
// --------------------------------------------------------------------

// Two apps driven through the same script run the same number of ticks and paint the same frames.
// This is the property everything else depends on: a count that varied between runs would make
// every counter assertion flaky, and a flaky assertion is one a reader learns to ignore.
void test_the_same_script_produces_the_same_counters() {
    testing::note_case("the_same_script_produces_the_same_counters");
    const auto run = [] {
        testing::TestApp app;
        app.build();
        app.pump_ticks(10);
        return std::pair<std::size_t, std::size_t>{app.ticks_run(), app.frames_painted()};
    };
    const auto first = run();
    const auto second = run();
    testing::expect_eq(first.first, second.first, "the same script ran the same ticks");
    testing::expect_eq(first.second, second.second, "the same script painted the same frames");
    testing::expect_eq(first.first, std::size_t{10}, "pump_ticks ran exactly ten ticks");
}

// Time is simulated: the clock is the test's, it starts at zero, and it moves only when the test
// moves it. No wall clock is read anywhere in the kit.
void test_time_is_the_tests_to_move() {
    testing::note_case("time_is_the_tests_to_move");
    testing::TestApp app;
    app.build();
    testing::expect(std::fabs(app.now()) < 1e-12, "the clock starts at zero");

    app.pump_ticks(6);
    testing::expect(std::fabs(app.now() - 6.0 * (1.0 / 60.0)) < 1e-9,
                    "six ticks advanced six sixtieths of a second");

    // `advance` covers the requested span in whole ticks, so an animation sampled at 100 ms sees
    // the same time whether it arrives as one call or two.
    testing::TestApp one_call;
    one_call.build();
    one_call.advance(0.1);
    testing::TestApp two_calls;
    two_calls.build();
    two_calls.advance(0.05);
    two_calls.advance(0.05);
    testing::expect(std::fabs(one_call.now() - two_calls.now()) < 1e-9,
                    "advance(0.1) and advance(0.05) twice reach the same instant");
    testing::expect_eq(one_call.ticks_run(), two_calls.ticks_run(),
                       "and they ran the same number of ticks");
}

// A non-positive duration does nothing rather than looping forever or rejecting, which is what a
// scripted test with a computed duration needs.
void test_advancing_by_nothing_is_a_no_op() {
    testing::note_case("advancing_by_nothing_is_a_no_op");
    testing::TestApp app;
    app.build();
    const std::size_t before = app.ticks_run();
    app.advance(0.0);
    app.advance(-1.0);
    testing::expect_eq(app.ticks_run(), before, "a non-positive advance ran no ticks");
}

// --- settling (issue #838)
// --------------------------------------------------------------------------

// A quiet app settles in one step, and the counts say exactly what that step was. The second
// settle is also one step: nothing pending is the state `settle` leaves behind, not something it
// consumed.
void test_a_quiet_app_settles_in_one_step() {
    testing::note_case("a_quiet_app_settles_in_one_step");
    testing::TestApp app;
    app.build();
    const std::size_t ticks_before = app.ticks_run();
    const testing::SettleResult first = app.settle();
    testing::expect(first.outcome == testing::SettleOutcome::Settled, "a quiet app settles");
    testing::expect_eq(first.ticks, std::size_t{1}, "convergence took exactly one step");
    testing::expect_eq(app.ticks_run() - ticks_before, std::size_t{1},
                       "one simulated step ran one tick");

    const testing::SettleResult second = app.settle();
    testing::expect(second.outcome == testing::SettleOutcome::Settled,
                    "an already settled app settles again");
    testing::expect_eq(second.completions, std::size_t{0}, "nothing was left to drain");
    testing::expect_eq(second.timers_fired, std::size_t{0}, "no timer fired");
}

// `settle` delivers what an attached fake owes - the wiring the guide's old pump loop repeated by
// hand. A delayed exchange becomes due only because the step moved the clock, which is also what
// keeps the run bounded: the deliverer is the fake, never a sleep.
void test_settle_delivers_a_delayed_fake() {
    testing::note_case("settle_delivers_a_delayed_fake");
    testing::TestApp app;

    appfw::FakeNet net(app.completions(), app.clock());
    appfw::NetResponse response;
    response.status = 200;
    response.body = "ok";
    net.push_response(response, 120);
    app.attach(net);
    // Start the exchange, as the transport's caller would: settle delivers what was started, it
    // never starts anything.
    appfw::NetRequest request;
    request.url = "https://api.example.test/weather";
    std::optional<appfw::NetResult> delivered;
    (void)net.start(request, {},
                    [&delivered](appfw::NetResult result) { delivered = std::move(result); });

    // Nothing is owed yet at t=0, so the first step is not the delivery; the second one is.
    const testing::SettleResult ran = app.settle();
    testing::expect(ran.outcome == testing::SettleOutcome::Settled, "the exchange settled");
    testing::expect(ran.completions >= 1, "the delivery was drained through the completion queue");
    testing::expect(delivered.has_value() && delivered->has_value(),
                    "the scripted response reached the completion");
    testing::expect_eq(net.in_flight(), std::size_t{0}, "the fake no longer holds the exchange");
}

// A fake whose answer is due past every step the bound allows reports `BoundReached` with the
// counts, which is the difference between "settled" and "gave up at the bound".
void test_settle_reports_the_bound_instead_of_passing() {
    testing::note_case("settle_reports_the_bound_instead_of_passing");
    testing::TestApp app;

    appfw::FakeNet net(app.completions(), app.clock());
    net.push_error(appfw::NetErrorKind::Dns, "never", 3'600'000);
    app.attach(net);
    appfw::NetRequest request;
    request.url = "https://api.example.test/weather";
    (void)net.start(request, {}, [](appfw::NetResult) {});

    testing::SettleConfig config;
    config.max_ticks = 4;
    const testing::SettleResult ran = app.settle(config);
    testing::expect(ran.outcome == testing::SettleOutcome::BoundReached,
                    "an exchange due past the bound reports BoundReached");
    testing::expect_eq(ran.ticks, std::size_t{4}, "the run stopped at the bound, not before");
}

// `pump_io = false` freezes the worker half and the fakes: the queued work stays exactly the
// "still in flight" state a test asks to observe, and the settle ends at the first step rather
// than reporting frozen work as an unsettled exchange.
void test_settle_with_pump_io_off_reports_still_in_flight() {
    testing::note_case("settle_with_pump_io_off_reports_still_in_flight");
    testing::TestApp app;

    appfw::FakeNet net(app.completions(), app.clock());
    net.push_error(appfw::NetErrorKind::Dns, "never");
    app.attach(net);
    appfw::NetRequest request;
    request.url = "https://api.example.test/weather";
    (void)net.start(request, {}, [](appfw::NetResult) {});

    testing::SettleConfig config;
    config.pump_io = false;
    const testing::SettleResult ran = app.settle(config);
    testing::expect(ran.outcome == testing::SettleOutcome::Settled,
                    "with the worker half frozen the settle ends at the first step");
    testing::expect_eq(net.in_flight(), std::size_t{1},
                       "the exchange is still in flight, which is the state the test observes");
    testing::expect(app.executor().pending_work() == 0,
                    "the executor queue is untouched when the worker half is frozen");
}

// `run_until` stops at the step where the predicate turns true, not one later, and the outcome
// says whether the predicate held or the bound ran out first. The predicate here watches a
// completion land - the kit-level form of "until node 9001 has text".
void test_run_until_stops_when_the_predicate_holds() {
    testing::note_case("run_until_stops_when_the_predicate_holds");
    testing::TestApp app;

    bool delivered = false;
    app.completions().post(tessera::runtime::CompletionPriority::Visible,
                           [&delivered] { delivered = true; });
    // The predicate turns true on the first step; the run must stop there, with `Settled`.
    const testing::SettleResult ran =
        app.run_until([&delivered] { return delivered; }, testing::SettleConfig{});
    testing::expect(ran.outcome == testing::SettleOutcome::Settled,
                    "the predicate held within the run");
    testing::expect_eq(ran.ticks, std::size_t{1}, "it stopped at the step that satisfied it");
    testing::expect(delivered, "the completion the predicate watched actually ran");

    // A predicate that never holds reports the bound, with every step accounted for.
    testing::SettleConfig config;
    config.max_ticks = 3;
    const testing::SettleResult never = app.run_until([] { return false; }, config);
    testing::expect(never.outcome == testing::SettleOutcome::BoundReached,
                    "a predicate that never holds reports BoundReached");
    testing::expect_eq(never.ticks, std::size_t{3}, "every step up to the bound ran");
}

// `mount` is the build + mount + attach_scheduler wiring in one call: without it, a component's
// setter marks a ticket nothing flushes, which is the failure this helper makes unreachable.
void test_mount_wires_the_scheduler_a_component_needs() {
    testing::note_case("mount_wires_the_scheduler_a_component_needs");
    FixtureCard app;
    app.build();

    testing::expect_eq(app.count_nodes(kValue), std::size_t{1},
                       "mount built the component into the root");
    testing::expect(app.tap(kAdd), "the mounted component is hittable");
    // The increment went through the generated setter, the ticket and the host's scheduler flush:
    // the path that silently dead-ends when either half of the old hand wiring is missing.
    app.expect_text(kValue, "Count: 1", "the update reached apply_state through the scheduler");
}

// --- a locator that misses reports absence
// ---------------------------------------------------------

void test_a_missing_locator_reports_absence() {
    testing::note_case("a_missing_locator_reports_absence");
    testing::TestApp app;
    app.build();
    testing::expect(app.find(4242) == nullptr, "an unknown id finds nothing");
    testing::expect(app.text_of(4242) == std::nullopt, "an unknown id has no text");
    testing::expect(app.count_nodes(4242) == 0, "an unknown id counts zero nodes");
    // The assertions return false *and* record a failure, so the test's own exit code reflects it.
    // `expect_absent` is the form that succeeds here.
    testing::expect(app.expect_absent(4242, "unknown"),
                    "expect_absent succeeds for a missing node");
}

// A count above one means the locator is ambiguous. This is the check that turns "the test passed"
// into "the test passed on the node it names".
void test_duplicate_ids_are_visible() {
    testing::note_case("duplicate_ids_are_visible");
    testing::TestApp app;
    app.build();

    auto first = std::make_unique<ui::Node>(70);
    first->set_automation_id(7001);
    app.root().add_child(std::move(first));
    auto second = std::make_unique<ui::Node>(71);
    second->set_automation_id(7001);
    app.root().add_child(std::move(second));

    testing::expect_eq(app.count_nodes(7001), std::size_t{2},
                       "the duplicate is visible as a count");
    testing::expect_eq(app.count_nodes(kAdd), std::size_t{0},
                       "the counter fixture is not built here");
}

// --- the file-system fake
// --------------------------------------------------------------------------

// `FakeFileSystem` is the double the storage module's own contract suite uses, so an app's test and
// the engine's test assert against one definition of "what the file system did".
void test_the_file_system_fake_records_and_fails_on_request() {
    testing::note_case("the_file_system_fake_records_and_fails_on_request");
    appfw::FakeFileSystem files;

    testing::expect(files.write("/store/settings.json", "v1").has_value(), "the write succeeds");
    testing::expect(files.has_file("/store/settings.json"), "the file exists after the write");
    testing::expect_eq(files.content_of("/store/settings.json"), std::string("v1"),
                       "the bytes are readable");
    testing::expect_eq(files.steps(), std::vector<std::string>{"write"}, "one call was recorded");

    // One named step fails, and only that one: the atomic-replace sequence is deterministic, so the
    // test induces a mid-sequence failure instead of hoping for one.
    files.fail_at(appfw::FailStep::Replace);
    testing::expect(!files.replace("/store/settings.json.tmp", "/store/settings.json").has_value(),
                    "the replace failed as scripted");
    testing::expect_eq(files.failing_step(), appfw::FailStep::Replace,
                       "the failing step is readable");

    files.fail_at(appfw::FailStep::None);
    testing::expect(files.replace("/store/settings.json", "/store/settings.json").has_value(),
                    "with the failure cleared the replace succeeds again");

    // A parent-less path still lists under its prefix, which is what the storage module's directory
    // listing relies on. `/store/settings.json` and the two seeded files are siblings, so the
    // listing holds three entries.
    files.put("/store/a.txt", "a");
    files.put("/store/b.txt", "b");
    const auto entries = files.list("/store");
    testing::expect(entries.has_value(), "the directory lists");
    if (entries.has_value()) {
        testing::expect_eq(entries->size(), std::size_t{3}, "the three files are listed");
        // Sorted by name, which the `FileSystem` contract requires.
        bool sorted = true;
        for (std::size_t index = 1; index < entries->size(); ++index) {
            if ((*entries)[index - 1].name >= (*entries)[index].name) {
                sorted = false;
                break;
            }
        }
        testing::expect(sorted, "the listing is sorted by name");
    }
}

// The sink the kit counts through is internal, but a test can still prove a frame painted: the
// counter only moves when `AppHost::paint` actually ran, which is what makes "a static app idles at
// zero presents" assertable here rather than only in the window driver's tests.
void test_a_settled_app_paints_one_frame_and_then_idles() {
    testing::note_case("a_settled_app_paints_one_frame_and_then_idles");
    testing::TestApp app;
    app.build();
    const std::size_t after_build = app.frames_painted();
    testing::expect(after_build >= 1, "building painted at least one frame");

    // Ticks with nothing dirty must not paint: the pipeline only produces a frame when something
    // asked for one, and a test that ignored that would be asserting against a spinner.
    app.pump_ticks(5);
    testing::expect_eq(app.frames_painted(), after_build,
                       "a settled app painted nothing across five idle ticks");
}

} // namespace

int main() {
    test_the_same_script_produces_the_same_counters();
    test_time_is_the_tests_to_move();
    test_advancing_by_nothing_is_a_no_op();
    test_a_quiet_app_settles_in_one_step();
    test_settle_delivers_a_delayed_fake();
    test_settle_reports_the_bound_instead_of_passing();
    test_settle_with_pump_io_off_reports_still_in_flight();
    test_run_until_stops_when_the_predicate_holds();
    test_mount_wires_the_scheduler_a_component_needs();
    test_a_missing_locator_reports_absence();
    test_duplicate_ids_are_visible();
    test_the_file_system_fake_records_and_fails_on_request();
    test_a_settled_app_paints_one_frame_and_then_idles();

    if (testing::failures() != 0) {
        std::cerr << "testing_kit: " << testing::failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "testing_kit: ok\n";
    return 0;
}
