// SPDX-License-Identifier: MIT
//
// The permissions contract suite, part two (issue #451, docs/specs/permissions-v1.md): platform
// failures, a refused request, an answer after destruction (section 5 rule 9), `refresh` after a
// Settings change (section 9 item 5) and the observation surface.
//
// Part one (`permissions_tests.cpp`) covers the state model, coalescing and the declaration rule.

#include "permissions_test_support.hpp"

namespace permissions_test::cases {

using appfw::Permission;
using appfw::PermissionState;

// A platform failure leaves the state where it was: rule 8 says the table moves only on a state
// answer, and guessing that a prompt was shown is exactly what that rule forbids.
void test_a_platform_failure_leaves_the_state_alone() {
    Fixture fixture;
    fixture.backend.push_failure("backend unavailable");
    const auto result = fixture.ask(appfw::Permission::Camera);
    expect(!result.ok(), "a platform failure is an error");
    expect(result.error.kind == appfw::PermissionErrorKind::Platform,
           "the error names the platform");
    expect(fixture.service->status(appfw::Permission::Camera) ==
               appfw::PermissionState::NotDetermined,
           "the state did not move");
}

// A back end that refuses the request at all (exhausted script) is `Unsupported`, and the pending
// entry is removed so a retry is not coalesced onto a request that will never finish.
void test_a_refused_request_does_not_leave_a_stuck_pending_entry() {
    Fixture fixture;
    // No script pushed: the fake refuses.
    const auto result = fixture.ask(appfw::Permission::Camera);
    expect(result.error.kind == appfw::PermissionErrorKind::Unsupported,
           "a refused request reports unsupported");

    // A retry must reach the platform, not coalesce onto the dead entry.
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    const auto retry = fixture.ask(appfw::Permission::Camera);
    expect(retry.state == appfw::PermissionState::Granted, "the retry was asked and granted");
}

// Spec section 5 rule 9: an answer that arrives after the service is gone is dropped, never
// delivered into freed state.
void test_an_answer_after_destruction_is_dropped() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted, 100);

    bool called = false;
    (void)fixture.service->submit(appfw::Permission::Camera, {}, {},
                                  [&called](appfw::PermissionResult) { called = true; });

    // Destroy the service while the prompt is up, then let the platform answer.
    fixture.service.reset();
    fixture.clock.advance(0.1);
    fixture.backend.deliver_due();
    (void)fixture.completions.run(16);

    expect(!called, "a late answer was not delivered to a destroyed service");
}

// --- 6. refresh (spec section 9 item 5)
// -------------------------------------------------------------

// The OS can change the answer while the app is backgrounded (the user visited Settings). `refresh`
// re-reads without prompting, and reports how many states moved.
void test_refresh_picks_up_a_change_made_in_settings() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Denied);
    (void)fixture.ask(appfw::Permission::Camera);
    expect(fixture.service->status(appfw::Permission::Camera) == appfw::PermissionState::Denied,
           "the app knows the denial");

    // The user grants it in Settings while the app is away.
    fixture.backend.set_state(appfw::Permission::Camera, appfw::PermissionState::Granted);
    const std::size_t changed = fixture.service->refresh();
    expect(changed == 1, "refresh reports the one change");
    expect(fixture.service->status(appfw::Permission::Camera) == appfw::PermissionState::Granted,
           "the state table picked it up");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1, "refresh did not prompt");
}

// --- 7. Observation
// ---------------------------------------------------------------------------------

void test_a_finished_ticket_reports_its_result() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    const appfw::PermissionTicket ticket =
        fixture.service->submit(appfw::Permission::Camera, {}, {}, {});
    fixture.settle();

    const std::optional<appfw::PermissionResult> stored = fixture.service->result_of(ticket);
    expect(stored.has_value(), "a finished ticket has a result");
    if (stored.has_value()) {
        expect(stored->state == appfw::PermissionState::Granted, "and it is the granted state");
    }
    expect(!fixture.service->result_of(appfw::PermissionTicket{999}).has_value(),
           "an unknown ticket has none");
}

// The record bound evicts the oldest terminal record; `max_records = 1` makes that observable.
void test_the_record_bound_evicts_the_oldest() {
    Fixture fixture(appfw::all_permissions_mask(), 1);
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    const appfw::PermissionTicket first =
        fixture.service->submit(appfw::Permission::Camera, {}, {}, {});
    fixture.settle();

    fixture.backend.push_outcome(appfw::PermissionState::Denied);
    const appfw::PermissionTicket second =
        fixture.service->submit(appfw::Permission::Microphone, {}, {}, {});
    fixture.settle();

    expect(!fixture.service->result_of(first).has_value(), "the oldest record was evicted");
    expect(fixture.service->result_of(second).has_value(), "the newest record is kept");
}

// The rationale is the app's text: the module passes it through and never writes prose.
void test_the_rationale_reaches_the_platform_unmodified() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    appfw::PermissionRequestOptions options;
    options.rationale = "To scan the code on the box.";
    (void)fixture.ask(appfw::Permission::Camera, 0, options);

    expect(fixture.backend.received().size() == 1, "one prompt was recorded");
    if (!fixture.backend.received().empty()) {
        expect(fixture.backend.received()[0].rationale == "To scan the code on the box.",
               "the app's sentence reached the platform verbatim");
    }
}

// The name table is the config parser's vocabulary, so a typo must not resolve.
void test_permission_names_round_trip_and_reject_typos() {
    for (const appfw::Permission permission :
         {appfw::Permission::Camera, appfw::Permission::Microphone, appfw::Permission::Location,
          appfw::Permission::Notifications, appfw::Permission::PhotoLibrary,
          appfw::Permission::FileAccess}) {
        appfw::Permission parsed = appfw::Permission::Camera;
        expect(appfw::permission_from_name(appfw::permission_name(permission), parsed),
               "a name round-trips");
        expect(parsed == permission, "and resolves to the same permission");
    }
    appfw::Permission ignored = appfw::Permission::Camera;
    expect(!appfw::permission_from_name("camra", ignored), "a typo does not resolve");
    expect(!appfw::permission_from_name("Camera", ignored), "the ids are case-sensitive");
}

// The two predicates the UI branches on. `askable` is false exactly for the states where offering
// "try again" would be a lie.
void test_askable_matches_the_states_that_can_prompt() {
    expect(appfw::permission_askable(appfw::PermissionState::NotDetermined),
           "never asked is askable");
    expect(appfw::permission_askable(appfw::PermissionState::Denied), "a denial is askable");
    expect(!appfw::permission_askable(appfw::PermissionState::Granted), "granted needs no ask");
    expect(!appfw::permission_askable(appfw::PermissionState::PermanentlyDenied),
           "a permanent denial cannot be asked");
    expect(!appfw::permission_askable(appfw::PermissionState::Restricted),
           "a policy refusal cannot be asked");
    expect(appfw::permission_usable(appfw::PermissionState::Granted), "granted is usable");
    expect(!appfw::permission_usable(appfw::PermissionState::Denied), "denied is not usable");
}

void run_edge_cases() {
    test_a_platform_failure_leaves_the_state_alone();
    test_a_refused_request_does_not_leave_a_stuck_pending_entry();
    test_an_answer_after_destruction_is_dropped();
    test_refresh_picks_up_a_change_made_in_settings();
    test_a_finished_ticket_reports_its_result();
    test_the_record_bound_evicts_the_oldest();
    test_the_rationale_reaches_the_platform_unmodified();
    test_permission_names_round_trip_and_reject_typos();
    test_askable_matches_the_states_that_can_prompt();
}

} // namespace permissions_test::cases

namespace permissions_test {
void run_permissions_edge_cases() {
    cases::run_edge_cases();
}
} // namespace permissions_test
