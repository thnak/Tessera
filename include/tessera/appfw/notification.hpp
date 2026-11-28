// SPDX-License-Identifier: MIT
//
// Notification value types and the platform seam (docs/specs/notifications-v1.md; issue #452).
//
// `NotificationBackend` is the whole platform boundary: a toast, a UserNotifications request, an
// Android channel post or a desktop-notification D-Bus call all reduce to "post this, cancel that,
// set the badge". The module owns every rule that must hold on every platform (consent, channel
// validation, deduplication, the pending bound, cold versus hot activation), so the contract suite
// pins them once against the in-tree fake instead of per platform.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// A category of notifications the user can silence as a group. Android requires one per
// notification; elsewhere it is carried as the category/thread id or ignored (spec section 7).
struct NotificationChannel {
    std::string id;
    std::string name;
    bool silent = false;
};

struct NotificationAction {
    std::string id;
    std::string title;
};

struct NotificationRequest {
    // The deduplication id (spec section 3 rule 2): scheduling the same id again replaces the
    // earlier notification instead of stacking a second one. Required.
    std::string id;
    std::string channel;
    std::string title;
    std::string body;
    // Milliseconds on the app's injected clock; 0 means "now". No wall clock is read here.
    std::uint64_t deliver_at_ms = 0;
    std::vector<NotificationAction> actions;
    // The deep link a tap opens (deep-linking-v1.md). Untrusted at activation time: it is parsed
    // against the app's allowlists then, not here.
    std::string link;
};

// Every outcome of `schedule` is a value. A denied permission is a status, never an error or an
// exception (permissions-v1.md section 4).
enum class NotificationStatus : std::uint8_t {
    Scheduled = 0,
    // Same id as an earlier one: the earlier notification was replaced.
    Replaced,
    // The Notifications permission is not Granted (or is not declared): nothing was posted.
    PermissionNotGranted,
    InvalidRequest,
    UnknownChannel,
    TooManyPending,
    // The back end cannot do what was asked (scheduling, actions, badge on this platform).
    Unsupported,
};

[[nodiscard]] constexpr std::string_view
notification_status_name(NotificationStatus status) noexcept {
    switch (status) {
    case NotificationStatus::Scheduled:
        return "scheduled";
    case NotificationStatus::Replaced:
        return "replaced";
    case NotificationStatus::PermissionNotGranted:
        return "permission-not-granted";
    case NotificationStatus::InvalidRequest:
        return "invalid-request";
    case NotificationStatus::UnknownChannel:
        return "unknown-channel";
    case NotificationStatus::TooManyPending:
        return "too-many-pending";
    case NotificationStatus::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

// What a tap on a notification (or one of its actions) normalizes to (spec section 5). The OS
// intent never reaches the UI; this value does.
struct NotificationActivation {
    std::string notification_id;
    // Empty for a tap on the body.
    std::string action_id;
    std::string link;
    // Set by the service, not the platform: true when no handler was installed yet, i.e. the app
    // was launched by the tap.
    bool cold_start = false;
};

struct NotificationBackendCapabilities {
    bool scheduling = true;
    bool actions = true;
    bool badge = true;
};

class NotificationBackend {
public:
    NotificationBackend() = default;
    NotificationBackend(const NotificationBackend&) = delete;
    NotificationBackend& operator=(const NotificationBackend&) = delete;
    virtual ~NotificationBackend() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual NotificationBackendCapabilities capabilities() const = 0;
    // Posts (now) or arms (deliver_at_ms > 0) one notification, replacing any with the same id.
    // False when the platform refused. Must not block the UI thread.
    virtual bool post(const NotificationRequest& request) = 0;
    virtual bool cancel(std::string_view id) = 0;
    virtual void cancel_all() = 0;
    virtual bool set_badge(std::uint32_t count) = 0;
};

} // namespace tessera::appfw
