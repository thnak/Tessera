// SPDX-License-Identifier: MIT
//
// The permissions contract suite, part one (issue #451, docs/specs/permissions-v1.md): the state
// model of section 4, coalescing (section 5 rule 5) and the declaration rule of section 7.
//
// Part two (`permissions_edges_tests.cpp`) covers failures, cancellation, staleness, refresh and
// observation. The split is the file-size rule, along a line the suite already had.
//
// Deterministic end to end: `ManualClock` for every instant, `FakePermissionBackend` for the
// platform and a hand-drained `CompletionQueue` for delivery, so no test prompts, reads a wall
// clock, sleeps, starts a thread or touches an OS permission.

#include "permissions_test_support.hpp"

namespace permissions_test::cases {

using appfw::Permission;
using appfw::PermissionState;

// --- 1. The state model (spec section 4)
// -----------------------------------------------------------

void test_a_grant_prompts_once_and_sticks() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted);

    const auto result = fixture.ask(appfw::Permission::Camera);
    expect(result.ok(), "a granted request is not an error");
    expect(result.state == appfw::PermissionState::Granted, "the state is granted");
    expect(fixture.service->status(appfw::Permission::Camera) == appfw::PermissionState::Granted,
           "the state table holds the answer");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "the platform was asked once");

    // Rule 2: asking again does not prompt.
    const auto again = fixture.ask(appfw::Permission::Camera);
    expect(again.state == appfw::PermissionState::Granted, "a granted permission stays granted");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "a granted permission was not asked again");
}

void test_each_refusal_state_is_distinct() {
    Fixture fixture;
    fixture.backend.push_denied();
    expect(fixture.ask(appfw::Permission::Microphone).state == appfw::PermissionState::Denied,
           "a denial is Denied");

    fixture.backend.push_permanently_denied();
    expect(fixture.ask(appfw::Permission::Location).state ==
               appfw::PermissionState::PermanentlyDenied,
           "a permanent denial is its own state");

    fixture.backend.push_restricted();
    expect(fixture.ask(appfw::Permission::Notifications).state ==
               appfw::PermissionState::Restricted,
           "a policy refusal is its own state");
}

// Rule 3: a permanently denied permission never prompts again. This is the rule that stops an app
// nagging a user it cannot persuade.
void test_a_permanently_denied_permission_never_prompts_again() {
    Fixture fixture;
    fixture.backend.push_permanently_denied();
    (void)fixture.ask(appfw::Permission::Camera);
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "the first prompt happened");

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = fixture.ask(appfw::Permission::Camera);
        expect(result.state == appfw::PermissionState::PermanentlyDenied,
               "a permanently denied permission resolves as such");
    }
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "no further prompt was shown");
    expect(fixture.service->stats().answered_without_prompt >= 3,
           "the answers came from the state table");
}

// Rule 4: a policy refusal cannot be retried, so it must not prompt either.
void test_a_restricted_permission_never_prompts_again() {
    Fixture fixture;
    fixture.backend.push_restricted();
    (void)fixture.ask(appfw::Permission::Camera);
    (void)fixture.ask(appfw::Permission::Camera);
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "a restricted permission was asked once only");
}

// Rule 7: an app that has shown its own "not now" opts out of the OS prompt.
void test_retry_after_denial_false_does_not_prompt() {
    Fixture fixture;
    fixture.backend.push_denied();
    (void)fixture.ask(appfw::Permission::Camera);

    appfw::PermissionRequestOptions options;
    options.retry_after_denial = false;
    const auto result = fixture.ask(appfw::Permission::Camera, 0, options);
    expect(result.state == appfw::PermissionState::Denied, "the denial is reported again");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "the opted-out retry did not prompt");
}

// With `retry_after_denial` left at its default, a denial IS retried: the two cases must differ, or
// the option is a no-op that reads like it works.
void test_retry_after_denial_true_prompts_again() {
    Fixture fixture;
    fixture.backend.push_denied();
    (void)fixture.ask(appfw::Permission::Camera);
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    const auto result = fixture.ask(appfw::Permission::Camera);
    expect(result.state == appfw::PermissionState::Granted, "the retry was granted");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 2, "the retry did prompt");
}

// --- 2. Coalescing (spec section 5 rule 5)
// ---------------------------------------------------------
//
// The issue's explicit acceptance criterion: a request while one is pending coalesces and never
// prompts twice.

void test_a_pending_request_coalesces() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted, 100);

    std::vector<appfw::PermissionResult> results;
    const auto collect = [&results](appfw::PermissionResult result) {
        results.push_back(std::move(result));
    };
    // Three callers, one prompt. The clock has not moved, so none of them has an answer yet.
    const appfw::PermissionTicket first =
        fixture.service->submit(appfw::Permission::Camera, {}, {}, collect);
    const appfw::PermissionTicket second =
        fixture.service->submit(appfw::Permission::Camera, {}, {}, collect);
    const appfw::PermissionTicket third =
        fixture.service->submit(appfw::Permission::Camera, {}, {}, collect);

    expect(results.empty(), "nothing resolved on the submit stack");
    expect(first == second && second == third, "the coalesced callers share one ticket");
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "only one prompt was shown");
    expect(fixture.service->stats().coalesced == 2, "two callers joined the first");

    fixture.clock.advance(0.1);
    fixture.settle();

    expect(results.size() == 3, "every coalesced caller got the one answer");
    for (const appfw::PermissionResult& result : results) {
        expect(result.state == appfw::PermissionState::Granted, "the shared answer is granted");
    }
    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1,
           "coalescing never prompted twice");
}

// A coalesced request for a *different* permission is a different request.
void test_two_permissions_prompt_separately() {
    Fixture fixture;
    fixture.backend.push_outcome(appfw::PermissionState::Granted);
    fixture.backend.push_outcome(appfw::PermissionState::Denied);

    std::vector<appfw::PermissionResult> results;
    const auto collect = [&results](appfw::PermissionResult result) {
        results.push_back(std::move(result));
    };
    (void)fixture.service->submit(appfw::Permission::Camera, {}, {}, collect);
    (void)fixture.service->submit(appfw::Permission::Microphone, {}, {}, collect);

    expect(fixture.backend.requests_for(appfw::Permission::Camera) == 1, "the camera prompted");
    expect(fixture.backend.requests_for(appfw::Permission::Microphone) == 1,
           "the microphone prompted separately");
    expect(fixture.service->stats().coalesced == 0, "nothing coalesced");

    fixture.settle();
    expect(results.size() == 2, "both resolved");
}

// --- 3. The declaration rule (spec section 7)
// -------------------------------------------------------

void test_an_undeclared_permission_is_refused_and_never_prompts() {
    // Only the camera is declared.
    Fixture fixture(appfw::permission_mask(appfw::Permission::Camera));
    const auto result = fixture.ask(appfw::Permission::Microphone);
    expect(!result.ok(), "an undeclared request is an error");
    expect(result.error.kind == appfw::PermissionErrorKind::NotDeclared,
           "the error names the declaration");
    expect(fixture.backend.requests_for(appfw::Permission::Microphone) == 0,
           "the platform was never asked");
    expect(fixture.service->stats().undeclared == 1, "the counter moved");
}

// No permission declared at all is the least-privilege default: every request is refused.
void test_an_app_that_declares_nothing_can_request_nothing() {
    Fixture fixture(0);
    for (const appfw::Permission permission :
         {appfw::Permission::Camera, appfw::Permission::Microphone, appfw::Permission::Location,
          appfw::Permission::Notifications, appfw::Permission::PhotoLibrary,
          appfw::Permission::FileAccess}) {
        const auto result = fixture.ask(permission);
        expect(result.error.kind == appfw::PermissionErrorKind::NotDeclared,
               "every permission is undeclared");
    }
    expect(fixture.backend.received().empty(), "the platform was asked nothing at all");
}

void test_a_declared_permission_is_allowed_through() {
    Fixture fixture(appfw::permission_mask(appfw::Permission::Notifications));
    expect(fixture.service->declared(appfw::Permission::Notifications),
           "the declared permission reads as declared");
    expect(!fixture.service->declared(appfw::Permission::Camera), "an undeclared one does not");
    expect(fixture.service->declared(appfw::Permission::Notifications), "and the first still does");
}

// --- 4. The platform rows (spec section 3)
// ----------------------------------------------------------

// A platform with no permission model: `Granted` by presence, no dialogue invented (spec section 9
// item 4). The module must not call `request` for it, because there is nothing to show.
void test_a_promptless_permission_is_granted_without_a_prompt() {
    Fixture fixture;
    // The capabilities are cached at construction (spec section 9 item 5: the module caches and a
    // `Resume` refreshes), so a test that models a different platform refreshes before asking. That
    // is the same call an app makes after the user changes something outside it.
    fixture.backend.set_promptless(appfw::Permission::FileAccess, true);
    (void)fixture.service->refresh();

    const auto result = fixture.ask(appfw::Permission::FileAccess);
    expect(result.ok(), "a promptless permission resolves cleanly");
    expect(result.state == appfw::PermissionState::Granted, "it is granted by presence");
    expect(fixture.backend.requests_for(appfw::Permission::FileAccess) == 0,
           "no prompt was invented");
}

void test_an_unsupported_permission_is_reported_not_granted() {
    Fixture fixture;
    fixture.backend.set_supported(appfw::Permission::PhotoLibrary, false);
    (void)fixture.service->refresh();

    const auto result = fixture.ask(appfw::Permission::PhotoLibrary);
    expect(!result.ok(), "an unsupported permission is an error");
    expect(result.error.kind == appfw::PermissionErrorKind::Unsupported,
           "the error names the capability");
    expect(fixture.backend.requests_for(appfw::Permission::PhotoLibrary) == 0,
           "the platform was never asked");
}
// One entry point per half, called by `permissions_main.cpp`. A plain function rather than a `main`
// in each TU: two executables would run the same fixture twice and report two pass lines for one
// suite, and one binary with two `main`s does not link.
void run_core_cases() {
    test_a_grant_prompts_once_and_sticks();
    test_each_refusal_state_is_distinct();
    test_a_permanently_denied_permission_never_prompts_again();
    test_a_restricted_permission_never_prompts_again();
    test_retry_after_denial_false_does_not_prompt();
    test_retry_after_denial_true_prompts_again();
    test_a_pending_request_coalesces();
    test_two_permissions_prompt_separately();
    test_an_undeclared_permission_is_refused_and_never_prompts();
    test_an_app_that_declares_nothing_can_request_nothing();
    test_a_declared_permission_is_allowed_through();
    test_a_promptless_permission_is_granted_without_a_prompt();
    test_an_unsupported_permission_is_reported_not_granted();
}

} // namespace permissions_test::cases

namespace permissions_test {
void run_permissions_core_cases() {
    cases::run_core_cases();
}
} // namespace permissions_test
