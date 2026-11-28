// SPDX-License-Identifier: MIT
//
// `NotificationService`: the app-facing notification policy (docs/specs/notifications-v1.md;
// issue #452). The back end is the platform half; everything below holds on every platform:
//
//   * **Consent** (section 4). Nothing is posted unless the `PermissionService` reports
//     `Notifications` as `Granted`. Refusal is a `NotificationStatus`, a value. The service never
//     prompts: asking is the app's call, through `PermissionService::submit`.
//   * **Channels and deduplication** (section 3). A request names a configured channel and a
//     non-empty id; the same id replaces the earlier notification.
//   * **Activation** (section 5). The platform reports a tap through `on_activation`. With a
//   handler
//     installed that is a hot resume; without one the activation is held and delivered, flagged
//     `cold_start`, when the handler is installed. `route_notification_activation` then feeds the
//     link through the same `install_deep_link` path as any other activation.
//   * **Revocation** (section 4 rule 3). `sync_consent` cancels everything pending once the
//     permission is no longer granted.
//
// Not here, narrowed in the spec (section 9): push tokens and push delivery (FCM, APNs, WNS), which
// need an OS and a provider account. Single-threaded: called on the UI thread, like the rest of
// the appfw modules.

#pragma once

#include "tessera/appfw/navigation_deep_link.hpp"
#include "tessera/appfw/notification.hpp"
#include "tessera/appfw/permission_service.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

struct NotificationConfig {
    std::vector<NotificationChannel> channels;
    // Bound on armed (future) notifications the service tracks.
    std::size_t max_pending = 64;
    // Bound on activations held before a handler exists; the oldest is dropped first.
    std::size_t max_held_activations = 4;
};

struct NotificationScheduleResult {
    NotificationStatus status = NotificationStatus::Scheduled;
    // The permission state the decision was made on, so a caller can offer Settings for
    // `PermanentlyDenied` without a second query.
    PermissionState permission = PermissionState::NotDetermined;

    [[nodiscard]] bool ok() const noexcept {
        return status == NotificationStatus::Scheduled || status == NotificationStatus::Replaced;
    }
};

class NotificationService {
public:
    using ActivationHandler = std::function<void(const NotificationActivation&)>;

    // `backend` and `permissions` are borrowed and must outlive the service.
    NotificationService(NotificationConfig config, NotificationBackend& backend,
                        PermissionService& permissions);

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    [[nodiscard]] NotificationScheduleResult schedule(const NotificationRequest& request);

    // True when a pending or shown notification with that id was cancelled.
    bool cancel(std::string_view id);
    void cancel_all();

    // Called by the platform adapter when an armed notification was shown.
    void on_delivered(std::string_view id);

    // Cancels all pending notifications when the permission is no longer granted. Returns how many.
    std::size_t sync_consent();

    [[nodiscard]] NotificationStatus set_badge(std::uint32_t count);

    void set_activation_handler(ActivationHandler handler);
    void on_activation(NotificationActivation activation);

    [[nodiscard]] const std::vector<NotificationRequest>& pending() const noexcept {
        return pending_;
    }
    [[nodiscard]] std::size_t held_activations() const noexcept {
        return held_.size();
    }
    [[nodiscard]] const NotificationChannel* find_channel(std::string_view id) const noexcept;

private:
    NotificationConfig config_;
    NotificationBackend* backend_ = nullptr;
    PermissionService* permissions_ = nullptr;
    std::vector<NotificationRequest> pending_;
    std::vector<NotificationActivation> held_;
    ActivationHandler handler_;
};

// Routes an activation's link through the deep-link path with `ActivationSource::Notification`,
// keeping the cold/hot flag. An empty link is `NotFound`: the tap had nowhere to go, which the host
// treats as "just open the app".
[[nodiscard]] std::expected<DeepLinkResult, NavigationError>
route_notification_activation(const RouteTable& table, NavController& controller,
                              const DeepLinkConfig& config,
                              const NotificationActivation& activation);

} // namespace tessera::appfw
