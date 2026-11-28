// SPDX-License-Identifier: MIT
#include "tessera/appfw/notification_fake.hpp"

#include <algorithm>

namespace tessera::appfw {

namespace {

std::vector<NotificationRequest>::iterator find_id(std::vector<NotificationRequest>& list,
                                                   std::string_view id) {
    return std::find_if(list.begin(), list.end(),
                        [id](const NotificationRequest& r) { return r.id == id; });
}

} // namespace

bool FakeNotificationBackend::post(const NotificationRequest& request) {
    ++post_calls_;
    if (refuse_posts_) {
        return false;
    }
    // Same id replaces, wherever the earlier one is.
    if (auto it = find_id(armed_, request.id); it != armed_.end()) {
        armed_.erase(it);
    }
    if (auto it = find_id(shown_, request.id); it != shown_.end()) {
        shown_.erase(it);
    }
    (request.deliver_at_ms == 0 ? shown_ : armed_).push_back(request);
    return true;
}

bool FakeNotificationBackend::fire(std::string_view id) {
    auto it = find_id(armed_, id);
    if (it == armed_.end()) {
        return false;
    }
    shown_.push_back(std::move(*it));
    armed_.erase(it);
    return true;
}

bool FakeNotificationBackend::cancel(std::string_view id) {
    bool removed = false;
    if (auto it = find_id(armed_, id); it != armed_.end()) {
        armed_.erase(it);
        removed = true;
    }
    if (auto it = find_id(shown_, id); it != shown_.end()) {
        shown_.erase(it);
        removed = true;
    }
    return removed;
}

void FakeNotificationBackend::cancel_all() {
    armed_.clear();
    shown_.clear();
}

bool FakeNotificationBackend::set_badge(std::uint32_t count) {
    badge_ = count;
    return true;
}

} // namespace tessera::appfw
