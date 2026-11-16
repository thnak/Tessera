// SPDX-License-Identifier: MIT
//
// The locale service (docs/specs/i18n-v1.md sections 1, 5 and 8.1; ADR-050 section 6): the resolved
// locale is published through AF4's `Observable<T>`, the epoch advances only on a real change, and
// a redundant request neither wakes the scheduler nor dirties anything.

#include "appfw_i18n_test_support.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace i18n_test {
namespace {

using tessera::appfw::LocaleSnapshot;

void the_initial_locale_is_resolved_before_any_subscriber() {
    Harness harness({"zz", "de"});
    // `zz` is unsupported, `de` is; the service starts on `de` with no setter call, so the first
    // frame is already correct instead of showing one frame of empty or default text.
    expect(harness.service.current().tag == "de", "the initial request list is resolved");
    expect(harness.service.epoch() == 0, "the initial resolution is not a change");
    expect(harness.service.direction() == TextDirection::LeftToRight, "de is left-to-right");

    // A subscriber sees the resolved locale immediately, through the observable's initial value.
    ChangeRecorder recorder;
    auto subscription = harness.service.changed().subscribe(&ChangeRecorder::on_change, &recorder);
    expect(harness.service.changed().get().tag == "de",
           "the observable's value is the resolved locale");
    expect(recorder.calls == 0, "subscribing does not deliver a change");
    (void)subscription;
}

void a_locale_change_delivers_once_per_flush() {
    Harness harness({"en"});
    ChangeRecorder recorder;
    auto subscription = harness.service.changed().subscribe(&ChangeRecorder::on_change, &recorder);

    expect(harness.service.set_requests({"de"}), "switching to de reports a change");
    expect(harness.service.epoch() == 1, "the epoch increments on a real change");
    // The notification is coalesced by AF4: the setter has not called anyone yet.
    expect(recorder.calls == 0, "a set() only schedules the notification");
    expect(harness.service.changed().dirty(), "the change is pending for the next flush");

    harness.scheduler.flush();
    expect(recorder.calls == 1, "one flush delivers one notification");
    expect(recorder.last_tag == "de", "the delivered value is the new locale");
    expect(recorder.last_epoch == 1, "the delivered epoch is the new one");
    expect(!harness.service.changed().dirty(), "the flush clears the pending mark");

    // Many setters, one flush: the same coalescing AF4 guarantees for any observable.
    expect(harness.service.set_requests({"ar"}), "switching to ar reports a change");
    expect(harness.service.set_requests({"en"}), "switching back to en reports a change");
    harness.scheduler.flush();
    expect(recorder.calls == 2, "two changes before one flush still deliver one notification");
    expect(recorder.last_tag == "en", "the delivered value is the last one set");
    (void)subscription;
}

void a_redundant_request_does_not_advance_the_epoch_or_wake_the_scheduler() {
    Harness harness({"en"});
    const std::uint64_t initial_epoch = harness.service.epoch();

    // The same request, and a request that resolves to the same locale: both are "no change". This
    // is what keeps a settings panel that forwards every keystroke from relaying out.
    expect(!harness.service.set_requests({"en"}), "the same request is not a change");
    expect(!harness.service.set_requests({"en_US"}),
           "a fallback to the same locale is not a change");
    expect(harness.service.epoch() == initial_epoch, "the epoch did not advance");
    expect(!harness.service.changed().dirty(), "nothing was scheduled for the next flush");
    expect(harness.scheduler.pending() == false, "the scheduler has no pending ticket");
}

void the_window_override_is_explicit_and_cleared_by_an_empty_request() {
    Harness harness({"en"});
    expect(harness.service.window_override().empty(), "there is no override initially");

    // The override is a request in front of the app list (section 8.1: "Per-window override is
    // explicit; app-global default otherwise").
    expect(harness.service.set_window_override("de"), "an override to de reports a change");
    expect(harness.service.current().tag == "de", "the override wins over the app list");
    expect(harness.service.window_override() == "de", "the override is retained for the app");

    // Clearing it falls back to the app-level request, which is a change again.
    expect(harness.service.set_window_override(""), "clearing the override reports a change");
    expect(harness.service.current().tag == "en", "clearing falls back to the app list");
    expect(!harness.service.set_window_override(""), "clearing an absent override is a no-op");

    // A supported override for a window that the app never requested is still a change.
    expect(harness.service.set_window_override("ar"), "an override to ar reports a change");
    expect(harness.service.right_to_left(), "the override's direction is published");
}

void the_service_resolves_text_through_its_catalog() {
    Harness harness({"de"});
    expect(harness.service.text("orders.title") == "Bestellungen",
           "text() serves the current locale");
    expect(harness.service.set_requests({"en"}), "switching back to en reports a change");
    expect(harness.service.text("orders.title") == "Orders",
           "text() follows the locale change without a rebuild");
    // A miss is visible text, never empty (section 8.1).
    expect(harness.service.text("orders.missing") == "orders.missing",
           "a missing key is the visible key");
    expect(harness.service.catalog().diagnostics().visible_keys == 1,
           "the service's catalog recorded the miss");
}

void a_direction_flip_is_a_locale_change() {
    Harness harness({"en"});
    expect(harness.service.direction() == TextDirection::LeftToRight, "en is LTR");
    expect(harness.service.set_requests({"ar"}), "switching to ar reports a change");
    expect(harness.service.direction() == TextDirection::RightToLeft, "ar is RTL");
    expect(harness.service.right_to_left(), "right_to_left() mirrors direction()");
    expect(harness.service.resolution().within_requested_language(),
           "the resolution stays within the requested language");
}

void the_snapshot_equality_is_the_resolved_locale() {
    // `Observable::set` coalesces with ==, so this is the rule that makes a redundant request free.
    const LocaleSnapshot first{"en", TextDirection::LeftToRight, 3};
    const LocaleSnapshot same{"en", TextDirection::LeftToRight, 9};
    const LocaleSnapshot other_tag{"de", TextDirection::LeftToRight, 3};
    const LocaleSnapshot other_direction{"en", TextDirection::RightToLeft, 3};
    expect(first == same, "the epoch is not part of snapshot equality");
    expect(!(first == other_tag), "a different tag is a different locale");
    expect(!(first == other_direction), "a different direction is a different locale");
}

void a_locale_change_leaves_the_request_list_intact() {
    Harness harness({"zz", "de"});
    expect(harness.service.current().tag == "de", "the second request resolves");
    // A per-window override that resolves does not discard the app list: clearing it returns to the
    // app's own resolution, including the same fallback behaviour.
    expect(harness.service.set_window_override("zh_Hant"), "an override reports a change");
    expect(harness.service.current().tag == "zh_Hant", "the override is served exactly");
    expect(harness.service.set_window_override(""), "clearing reports a change");
    expect(harness.service.current().tag == "de", "the app list is still resolved");
    expect(harness.service.resolution().requested == "de",
           "the app list's request list is still resolved the same way");
}

} // namespace

int run_service_tests() {
    const int before = g_failures;
    the_initial_locale_is_resolved_before_any_subscriber();
    a_locale_change_delivers_once_per_flush();
    a_redundant_request_does_not_advance_the_epoch_or_wake_the_scheduler();
    the_window_override_is_explicit_and_cleared_by_an_empty_request();
    the_service_resolves_text_through_its_catalog();
    a_direction_flip_is_a_locale_change();
    the_snapshot_equality_is_the_resolved_locale();
    a_locale_change_leaves_the_request_list_intact();
    return g_failures - before;
}

} // namespace i18n_test
