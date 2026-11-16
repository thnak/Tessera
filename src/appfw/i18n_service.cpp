// SPDX-License-Identifier: MIT
//
// The i18n service (docs/specs/i18n-v1.md sections 1, 5 and 8.1; issue #195).

#include "tessera/appfw/i18n_service.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

LocaleService::LocaleService(I18nServiceConfig config, ui::UpdateScheduler& scheduler)
    : locales_(std::move(config.locales)),
      catalog_(std::move(config.catalog)),
      requests_(std::move(config.initial_requests)),
      resolution_(resolve_requests(locales_, requests_, window_override_)),
      current_{resolution_.resolved, resolution_.direction, 0},
      changed_(scheduler, current_) {}

LocaleResolution LocaleService::resolve_requests(const LocaleTable& locales,
                                                 const std::vector<std::string>& requests,
                                                 std::string_view window_override) {
    std::vector<std::string_view> views;
    views.reserve(requests.size() + 1);
    if (!window_override.empty()) {
        views.push_back(window_override);
    }
    for (const std::string& request : requests) {
        views.push_back(request);
    }
    return locales.resolve(views);
}

bool LocaleService::resolve_current() {
    const LocaleResolution next = resolve_requests(locales_, requests_, window_override_);
    const LocaleSnapshot candidate{next.resolved, next.direction, current_.epoch + 1};
    // `Observable::set` compares with ==, and LocaleSnapshot's operator== is "same resolved locale
    // and direction". A redundant request therefore updates nothing: no epoch, no wake, no
    // relayout.
    if (!changed_.set(candidate)) {
        resolution_ = next;
        return false;
    }
    resolution_ = next;
    current_ = candidate;
    return true;
}

bool LocaleService::set_requests(const std::vector<std::string>& requests) {
    requests_ = requests;
    return resolve_current();
}

bool LocaleService::set_window_override(std::string_view request) {
    if (request == window_override_) {
        return false;
    }
    window_override_ = std::string(request);
    return resolve_current();
}

std::string_view LocaleService::text(std::string_view id) const {
    return catalog_.text(id, current_.tag);
}

} // namespace tessera::appfw
