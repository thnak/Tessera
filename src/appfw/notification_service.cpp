// SPDX-License-Identifier: MIT
#include "tessera/appfw/notification_service.hpp"

#include <algorithm>
#include <utility>

namespace tessera::appfw {

NotificationService::NotificationService(NotificationConfig config, NotificationBackend& backend,
                                         PermissionService& permissions)
    : config_(std::move(config)),
      backend_(&backend),
      permissions_(&permissions) {}

const NotificationChannel* NotificationService::find_channel(std::string_view id) const noexcept {
    for (const NotificationChannel& channel : config_.channels) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

NotificationScheduleResult NotificationService::schedule(const NotificationRequest& request) {
    NotificationScheduleResult result;
    result.permission = permissions_->status(Permission::Notifications);
    // Consent first: a denied app learns nothing about its other arguments, and nothing is posted.
    if (!permissions_->declared(Permission::Notifications) ||
        !permission_usable(result.permission)) {
        result.status = NotificationStatus::PermissionNotGranted;
        return result;
    }
    if (request.id.empty() || (request.title.empty() && request.body.empty())) {
        result.status = NotificationStatus::InvalidRequest;
        return result;
    }
    if (find_channel(request.channel) == nullptr) {
        result.status = NotificationStatus::UnknownChannel;
        return result;
    }
    const NotificationBackendCapabilities caps = backend_->capabilities();
    if ((request.deliver_at_ms != 0 && !caps.scheduling) ||
        (!request.actions.empty() && !caps.actions)) {
        result.status = NotificationStatus::Unsupported;
        return result;
    }
    auto existing = std::find_if(pending_.begin(), pending_.end(),
                                 [&](const NotificationRequest& r) { return r.id == request.id; });
    if (request.deliver_at_ms != 0 && existing == pending_.end() &&
        pending_.size() >= config_.max_pending) {
        result.status = NotificationStatus::TooManyPending;
        return result;
    }
    if (!backend_->post(request)) {
        result.status = NotificationStatus::Unsupported;
        return result;
    }
    result.status = NotificationStatus::Scheduled;
    if (existing != pending_.end()) {
        pending_.erase(existing);
        result.status = NotificationStatus::Replaced;
    }
    if (request.deliver_at_ms != 0) {
        pending_.push_back(request);
    }
    return result;
}

bool NotificationService::cancel(std::string_view id) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [id](const NotificationRequest& r) { return r.id == id; });
    const bool was_pending = it != pending_.end();
    if (was_pending) {
        pending_.erase(it);
    }
    return backend_->cancel(id) || was_pending;
}

void NotificationService::cancel_all() {
    pending_.clear();
    backend_->cancel_all();
}

void NotificationService::on_delivered(std::string_view id) {
    std::erase_if(pending_, [id](const NotificationRequest& r) { return r.id == id; });
}

std::size_t NotificationService::sync_consent() {
    if (permission_usable(permissions_->status(Permission::Notifications))) {
        return 0;
    }
    const std::size_t cancelled = pending_.size();
    cancel_all();
    return cancelled;
}

NotificationStatus NotificationService::set_badge(std::uint32_t count) {
    if (!permission_usable(permissions_->status(Permission::Notifications))) {
        return NotificationStatus::PermissionNotGranted;
    }
    if (!backend_->capabilities().badge || !backend_->set_badge(count)) {
        return NotificationStatus::Unsupported;
    }
    return NotificationStatus::Scheduled;
}

void NotificationService::set_activation_handler(ActivationHandler handler) {
    handler_ = std::move(handler);
    if (!handler_) {
        return;
    }
    // Swap first so a handler that re-enters `on_activation` sees an empty queue.
    std::vector<NotificationActivation> held;
    held.swap(held_);
    for (NotificationActivation& activation : held) {
        activation.cold_start = true;
        handler_(activation);
    }
}

void NotificationService::on_activation(NotificationActivation activation) {
    activation.cold_start = false;
    if (handler_) {
        handler_(activation);
        return;
    }
    if (held_.size() >= config_.max_held_activations && !held_.empty()) {
        held_.erase(held_.begin());
    }
    held_.push_back(std::move(activation));
}

std::expected<DeepLinkResult, NavigationError>
route_notification_activation(const RouteTable& table, NavController& controller,
                              const DeepLinkConfig& config,
                              const NotificationActivation& activation) {
    if (activation.link.empty()) {
        return std::unexpected(NavigationError{NavigationErrorCode::NotFound, "link", 0});
    }
    return install_deep_link(table, controller, config, activation.link,
                             ActivationSource::Notification, activation.cold_start);
}

} // namespace tessera::appfw
