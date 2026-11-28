// SPDX-License-Identifier: MIT
//
// The notification contract suite (docs/specs/notifications-v1.md section 10; issue #452). Fake
// notification and permission back ends, a manual clock, a hand-drained completion queue and a
// route table: no OS, no wall clock, no network.

#include "appfw_navigation_test_support.hpp"
#include "permissions_test_support.hpp"
#include "tessera/appfw/notification_fake.hpp"
#include "tessera/appfw/notification_service.hpp"

#include <iostream>

namespace {

namespace appfw = tessera::appfw;
using nav_test::expect;

struct NotifFixture {
    explicit NotifFixture(std::uint64_t declared = appfw::all_permissions_mask())
        : permissions(declared) {
        appfw::NotificationConfig config;
        config.channels = {{"orders", "Orders", false}, {"promo", "Promotions", true}};
        config.max_pending = 2;
        service =
            std::make_unique<appfw::NotificationService>(config, backend, *permissions.service);
    }

    void grant() {
        permissions.backend.push_outcome(appfw::PermissionState::Granted);
        (void)permissions.ask(appfw::Permission::Notifications);
    }

    permissions_test::Fixture permissions;
    appfw::FakeNotificationBackend backend;
    std::unique_ptr<appfw::NotificationService> service;
};

appfw::NotificationRequest make_request(std::string id, std::uint64_t at_ms = 0) {
    appfw::NotificationRequest request;
    request.id = std::move(id);
    request.channel = "orders";
    request.title = "Order shipped";
    request.body = "Order 42 is on its way";
    request.deliver_at_ms = at_ms;
    request.link = "myapp://orders/42";
    return request;
}

void schedule_then_cancel() {
    NotifFixture f;
    f.grant();
    const auto scheduled = f.service->schedule(make_request("n1", 5000));
    expect(scheduled.status == appfw::NotificationStatus::Scheduled, "a granted app schedules");
    expect(f.backend.armed().size() == 1 && f.service->pending().size() == 1,
           "the notification is armed and tracked");
    expect(f.service->cancel("n1"), "cancel reports it removed one");
    expect(f.backend.armed().empty() && f.service->pending().empty(), "nothing remains armed");
    expect(!f.service->cancel("n1"), "cancelling again reports nothing removed");

    // Delivery by the OS drops it from the pending set.
    expect(f.service->schedule(make_request("n2", 100)).ok(), "a second one schedules");
    expect(f.backend.fire("n2"), "the fake fires it");
    f.service->on_delivered("n2");
    expect(f.service->pending().empty() && f.backend.shown().size() == 1, "delivered, not pending");
}

void deduplication_and_validation() {
    NotifFixture f;
    f.grant();
    expect(f.service->schedule(make_request("n1", 100)).status ==
               appfw::NotificationStatus::Scheduled,
           "first");
    auto again = make_request("n1", 200);
    again.body = "updated";
    expect(f.service->schedule(again).status == appfw::NotificationStatus::Replaced,
           "the same id replaces");
    expect(f.backend.armed().size() == 1 && f.backend.armed()[0].body == "updated",
           "one armed notification with the new body");

    auto bad_channel = make_request("n3");
    bad_channel.channel = "nope";
    expect(f.service->schedule(bad_channel).status == appfw::NotificationStatus::UnknownChannel,
           "an unconfigured channel is refused");
    expect(f.service->schedule(make_request("")).status ==
               appfw::NotificationStatus::InvalidRequest,
           "an empty id is invalid");

    expect(f.service->schedule(make_request("n4", 300)).ok(), "second distinct pending");
    expect(f.service->schedule(make_request("n5", 400)).status ==
               appfw::NotificationStatus::TooManyPending,
           "the pending bound holds");
    expect(f.service->schedule(make_request("n6")).ok(), "an immediate one is not bounded");

    f.backend.set_capabilities({false, false, false});
    expect(f.service->schedule(make_request("n7", 500)).status ==
               appfw::NotificationStatus::Unsupported,
           "scheduling on a back end without it is Unsupported");
    expect(f.service->set_badge(3) == appfw::NotificationStatus::Unsupported,
           "so is a badge without badge support");
}

void denied_permission_returns_a_value() {
    NotifFixture f;
    auto result = f.service->schedule(make_request("n1"));
    expect(result.status == appfw::NotificationStatus::PermissionNotGranted &&
               result.permission == appfw::PermissionState::NotDetermined,
           "before any answer: a value, nothing posted");
    expect(f.backend.post_calls() == 0, "the back end was never called");

    f.permissions.backend.push_permanently_denied();
    (void)f.permissions.ask(appfw::Permission::Notifications);
    result = f.service->schedule(make_request("n1"));
    expect(result.status == appfw::NotificationStatus::PermissionNotGranted &&
               result.permission == appfw::PermissionState::PermanentlyDenied,
           "denied: the status carries the permission state");
    expect(f.service->set_badge(1) == appfw::NotificationStatus::PermissionNotGranted,
           "a badge needs consent too");
    expect(f.backend.post_calls() == 0 && f.backend.badge() == 0, "still nothing posted");

    NotifFixture undeclared(0);
    expect(undeclared.service->schedule(make_request("n1")).status ==
               appfw::NotificationStatus::PermissionNotGranted,
           "an undeclared permission is refused as a value");
}

void revocation_cancels_pending() {
    NotifFixture f;
    f.grant();
    expect(f.service->schedule(make_request("n1", 100)).ok(), "armed");
    expect(f.service->sync_consent() == 0, "still granted: nothing cancelled");
    f.permissions.backend.set_state(appfw::Permission::Notifications,
                                    appfw::PermissionState::Denied);
    (void)f.permissions.service->refresh();
    expect(f.service->sync_consent() == 1, "revoked: the pending one is cancelled");
    expect(f.backend.armed().empty(), "and withdrawn from the platform");
}

void cold_tap_routes_to_a_deep_link() {
    NotifFixture f;
    nav_test::Harness harness;
    // The platform reports the tap before the app has a handler: the app was launched by it.
    f.service->on_activation({"n1", "", "myapp://orders/42", false});
    expect(f.service->held_activations() == 1, "the activation is held, not lost");

    std::vector<appfw::NotificationActivation> seen;
    f.service->set_activation_handler(
        [&](const appfw::NotificationActivation& a) { seen.push_back(a); });
    expect(seen.size() == 1 && seen[0].cold_start, "delivered once the handler exists, as cold");
    expect(f.service->held_activations() == 0, "the queue is drained");

    const auto routed = route_notification_activation(harness.table, harness.controller,
                                                      nav_test::link_config(), seen[0]);
    expect(routed.has_value() && routed->outcome == appfw::DeepLinkOutcome::Synthesized,
           "a cold tap synthesizes a stack");
    expect(harness.controller.depth() == 2 &&
               harness.controller.active()->route_id == "orders.detail",
           "root plus the target");
}

void hot_tap_routes_to_a_deep_link() {
    NotifFixture f;
    nav_test::Harness harness;
    (void)harness.controller.push("/settings");
    std::vector<appfw::NotificationActivation> seen;
    f.service->set_activation_handler(
        [&](const appfw::NotificationActivation& a) { seen.push_back(a); });
    f.service->on_activation({"n1", "open", "myapp://orders/42", true});
    expect(seen.size() == 1 && !seen[0].cold_start && seen[0].action_id == "open",
           "a running app is hot, whatever the platform claimed");

    const auto routed = route_notification_activation(harness.table, harness.controller,
                                                      nav_test::link_config(), seen[0]);
    expect(routed.has_value() && routed->outcome == appfw::DeepLinkOutcome::Pushed,
           "a hot tap pushes");
    expect(harness.controller.depth() == 2, "onto the existing stack");

    const auto hostile =
        route_notification_activation(harness.table, harness.controller, nav_test::link_config(),
                                      {"n2", "", "evil://orders/1", false});
    expect(!hostile.has_value(), "a link outside the allowlist is refused, as a value");
    const auto empty = route_notification_activation(
        harness.table, harness.controller, nav_test::link_config(), {"n3", "", "", false});
    expect(!empty.has_value(), "no link is a value too");
}

void held_activations_are_bounded() {
    NotifFixture f;
    for (int i = 0; i < 6; ++i) {
        f.service->on_activation({"n" + std::to_string(i), "", "", false});
    }
    expect(f.service->held_activations() == 4, "the oldest are dropped past the bound");
}

void fake_delivery_paths() {
    NotifFixture f;
    f.grant();
    // An immediate notification is shown at once and is never tracked as pending.
    expect(f.service->schedule(make_request("now")).ok(), "immediate posts");
    expect(f.backend.shown().size() == 1 && f.service->pending().empty(), "shown, not pending");
    // Re-posting a shown id replaces it rather than duplicating it.
    auto updated = make_request("now");
    updated.body = "newer";
    expect(f.service->schedule(updated).ok(), "re-post");
    expect(f.backend.shown().size() == 1 && f.backend.shown()[0].body == "newer",
           "a shown id is replaced in place");

    // A platform refusal surfaces as a value and leaves no pending state behind.
    f.backend.set_refuse_posts(true);
    const auto refused = f.service->schedule(make_request("later", 900));
    expect(refused.status == appfw::NotificationStatus::Unsupported && f.service->pending().empty(),
           "a refused post is Unsupported and not tracked");
    f.backend.set_refuse_posts(false);

    // Firing is by id only; an unknown id reports false and changes nothing.
    expect(f.service->schedule(make_request("a", 100)).ok() &&
               f.service->schedule(make_request("b", 200)).ok(),
           "two armed");
    expect(!f.backend.fire("missing") && f.backend.armed().size() == 2, "unknown id is a no-op");
    expect(f.backend.fire("b") && f.backend.armed().size() == 1, "fire moves one to shown");

    expect(f.service->set_badge(7) == appfw::NotificationStatus::Scheduled &&
               f.backend.badge() == 7,
           "a badge reaches the back end");
    f.service->cancel_all();
    expect(f.backend.armed().empty() && f.backend.shown().empty() && f.service->pending().empty(),
           "cancel_all clears armed, shown and pending");
}

} // namespace

int main() {
    fake_delivery_paths();
    schedule_then_cancel();
    deduplication_and_validation();
    denied_permission_returns_a_value();
    revocation_cancels_pending();
    cold_tap_routes_to_a_deep_link();
    hot_tap_routes_to_a_deep_link();
    held_activations_are_bounded();
    const int failures = nav_test::g_failures + permissions_test::failures;
    if (failures != 0) {
        std::cerr << "notification: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "notification: ok\n";
    return 0;
}
