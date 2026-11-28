// SPDX-License-Identifier: MIT

#include "tessera/appfw/router_service.hpp"

namespace tessera::appfw {

void NavRouterService::push(std::string_view path) {
    const auto result = controller_->push(path);
    last_error_ = result ? NavigationErrorCode::None : result.error().code;
}

void NavRouterService::replace(std::string_view path) {
    const auto result = controller_->replace(path);
    last_error_ = result ? NavigationErrorCode::None : result.error().code;
}

void NavRouterService::back() {
    // `back()` never fails: at-root and empty are reported outcomes, and the host applies its exit
    // policy, so there is nothing to record.
    (void)controller_->back();
    last_error_ = NavigationErrorCode::None;
}

} // namespace tessera::appfw
