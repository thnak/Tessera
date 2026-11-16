// SPDX-License-Identifier: MIT
//
// Redirect rules for the `net` module (docs/specs/networking-v1.md section 3 item 2; issue #197).
// See net_redirect.hpp for why the module owns this and the back end does not.

#include "tessera/appfw/net_redirect.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {

bool is_followable_redirect(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const std::string* redirect_location(const NetResponse& response) noexcept {
    return find_header(response.headers, "location");
}

std::expected<NetRequest, NetError>
follow_redirect(const NetRequest& current, const NetResponse& response, std::uint32_t followed) {
    if (followed >= kMaxRedirects) {
        return std::unexpected(NetError{NetErrorKind::TooManyRedirects,
                                        "more than the maximum number of redirects", 0});
    }
    const std::string* location = redirect_location(response);
    if (location == nullptr || location->empty()) {
        // A 3xx with no `Location` is not a failure: there is simply nothing to follow, so the
        // caller receives it as a response.
        return std::unexpected(NetError{NetErrorKind::Other, "a redirect without a Location", 0});
    }
    // A relative reference needs RFC 3986 resolution, which this slice does not implement
    // (net_url.hpp states the gap). Refusing is better than guessing an origin to send the next
    // hop - and the next hop would otherwise be a request the caller never approved.
    if (location->find("://") == std::string::npos) {
        return std::unexpected(
            NetError{NetErrorKind::Unsupported, "a relative redirect target is not supported", 0});
    }
    auto target = parse_url(*location);
    if (!target.has_value()) {
        return std::unexpected(NetError{
            target.error().kind, "a redirect target that does not parse", target.error().os_code});
    }
    auto origin = parse_url(current.url);
    if (!origin.has_value()) {
        // `current.url` was accepted by `send`, so this is a caller that mutated the request after
        // submitting it; refusing is the only safe answer.
        return std::unexpected(
            NetError{NetErrorKind::BadUrl, "the current request URL does not parse", 0});
    }
    if (origin->scheme == "https" && target->scheme == "http") {
        return std::unexpected(
            NetError{NetErrorKind::Unsupported, "an https to http redirect is refused", 0});
    }

    NetRequest next = current;
    next.url = url_string(*target);
    if (response.status == 301 || response.status == 302 || response.status == 303) {
        // The historical browsers' rule, which section 3 item 2 adopts verbatim: the method becomes
        // GET and the body is dropped. `HEAD` is not a `POST`, so it stays `HEAD` - the spec names
        // only `POST`.
        if (next.method == HttpMethod::Post) {
            next.method = HttpMethod::Get;
            next.body.clear();
            next.has_body = false;
        }
    }
    // `307`/`308` keep both, so nothing to do. The remaining statuses never reach here.
    if (origin_of(*origin) != origin_of(*target)) {
        // Section 3 item 2: a credential is scoped to the origin that issued it. Dropping the
        // header (rather than forwarding it) is what keeps a redirect from leaking a token.
        remove_header(next.headers, "authorization");
    }
    return next;
}

} // namespace tessera::appfw
