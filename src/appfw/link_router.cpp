// SPDX-License-Identifier: MIT

#include "tessera/appfw/link_router.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tessera::appfw {

struct LinkRouter::State {
    LinkDiagnostics counters;
};

namespace {

// RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) followed by ':'. Empty when `href`
// does not start with one (a path, a fragment, a relative reference).
std::string scheme_of(std::string_view href) {
    const std::size_t colon = href.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return {};
    }
    std::string scheme;
    for (std::size_t i = 0; i < colon; ++i) {
        const auto c = static_cast<unsigned char>(href[i]);
        const bool first_ok = std::isalpha(c) != 0;
        const bool rest_ok = std::isalnum(c) != 0 || c == '+' || c == '-' || c == '.';
        if (i == 0 ? !first_ok : !rest_ok) {
            return {};
        }
        scheme.push_back(static_cast<char>(std::tolower(c)));
    }
    return scheme;
}

} // namespace

LinkRouter::LinkRouter(NavController* navigation, ShellService* shell, LinkPolicy policy)
    : navigation_(navigation),
      shell_(shell),
      policy_(std::move(policy)),
      state_(std::make_shared<State>()) {}

LinkDiagnostics LinkRouter::diagnostics() const {
    return state_->counters;
}

LinkResult LinkRouter::open(std::string_view href) {
    LinkDiagnostics& counters = state_->counters;
    if (href.empty() || href.size() > kMaxShellUrlBytes || href.starts_with("//")) {
        ++counters.malformed;
        return {LinkOutcome::Malformed, "href is empty, too long or protocol-relative"};
    }
    if (href.front() == '/') {
        if (navigation_ == nullptr) {
            ++counters.service_unavailable;
            return {LinkOutcome::ServiceUnavailable, "no navigation controller"};
        }
        auto pushed = navigation_->push(href);
        if (!pushed) {
            ++counters.unresolved_path;
            return {LinkOutcome::UnresolvedPath,
                    std::string(navigation_error_code_name(pushed.error().code))};
        }
        ++counters.navigated;
        return {LinkOutcome::Navigated, {}};
    }
    const std::string scheme = scheme_of(href);
    if (scheme.empty()) {
        ++counters.malformed;
        return {LinkOutcome::Malformed, "href is neither an absolute path nor a URL"};
    }
    if (std::ranges::find(policy_.external_schemes, scheme) == policy_.external_schemes.end()) {
        ++counters.scheme_not_opened;
        return {LinkOutcome::SchemeNotOpened, "scheme '" + scheme + "' is not opened"};
    }
    if (shell_ == nullptr) {
        ++counters.service_unavailable;
        return {LinkOutcome::ServiceUnavailable, "no shell service"};
    }
    std::weak_ptr<State> weak = state_;
    auto submitted =
        shell_->submit_open_url(std::string(href), [weak](std::expected<void, ShellError> done) {
            if (auto state = weak.lock(); state != nullptr && !done) {
                ++state->counters.external_failed;
            }
        });
    if (!submitted) {
        ++counters.service_unavailable;
        return {LinkOutcome::ServiceUnavailable,
                std::string(shell_error_kind_name(submitted.error().kind))};
    }
    ++counters.opened_external;
    return {LinkOutcome::OpenedExternal, {}};
}

} // namespace tessera::appfw
