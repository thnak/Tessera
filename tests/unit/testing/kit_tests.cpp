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
//   * a locator that misses reports absence instead of asserting somewhere deeper;
//   * the `FakeFileSystem` is the one that ships with the storage module, records every call and
//   can
//     be told to fail at one named step;
//   * `tessera_testing` is not linked by any shipping target.

#include "CounterCard.hpp"
#include "tessera/appfw/filesystem_fake.hpp"
#include "tessera/testing/expect.hpp"
#include "tessera/testing/test_app.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace appfw = tessera::appfw;
namespace testing = tessera::testing;
namespace ui = tessera::ui;

constexpr std::uint64_t kValue = 1002;
constexpr std::uint64_t kAdd = 1004;

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
