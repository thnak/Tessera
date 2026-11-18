// SPDX-License-Identifier: MIT
//
// Deep-link parsing and installation (docs/specs/deep-linking-v1.md sections 2, 3, 5, 7 and 8;
// docs/specs/navigation-v1.md section 5; issue #196).

#include "tessera/appfw/navigation_deep_link.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {
namespace {

std::string as_lower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char character : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return lowered;
}

// deep-linking-v1.md section 3's scheme grammar `[a-z][a-z0-9+.-]*`, in its canonical lowercase
// form.
bool is_valid_scheme(std::string_view scheme) noexcept {
    if (scheme.empty() || scheme.front() < 'a' || scheme.front() > 'z') {
        return false;
    }
    for (const char character : scheme) {
        const bool alnum =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!alnum && character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

bool allowlisted(const std::vector<std::string>& list, std::string_view value) {
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string& entry) { return entry == value; });
}

struct ParsedUrl {
    std::string scheme;
    std::string host;
    // Owned, not a view: the custom-scheme form prepends a separator, so the canonical path is not
    // always a substring of the input and a view into a temporary would dangle.
    std::string path;
    std::string_view query; // without the `?`, always a substring of the input
    bool has_authority = false;
};

// Splits the URL. Two shapes, because the spec's own examples are both:
//
//   * `https://app.example.com/orders/42` - an App/Universal Link, where the authority is a real
//   host and
//     section 3's `hosts` allowlist is load-bearing;
//   * `myapp://orders/42` - a custom scheme, where the "host" in URI grammar is actually the first
//   route
//     segment. Everyone writes and reads it that way, so the whole remainder is the path.
//
// A custom scheme is therefore recognized by name rather than by the presence of a `.` or a port:
// `http` and `https` are the two schemes whose first segment is a host, and an app that registers
// another host-based scheme declares its hosts and gets the same treatment.
ParsedUrl split_url(std::string_view text, bool authority_is_host) {
    ParsedUrl parsed;
    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos) {
        const std::size_t query_at = text.find('?');
        if (query_at == std::string_view::npos) {
            parsed.path = std::string(text);
        } else {
            parsed.path = std::string(text.substr(0, query_at));
            parsed.query = text.substr(query_at + 1);
        }
        if (parsed.path.empty()) {
            parsed.path = "/";
        }
        return parsed;
    }
    parsed.has_authority = true;
    parsed.scheme = as_lower(text.substr(0, scheme_end));
    std::string_view remainder = text.substr(scheme_end + 3);
    if (authority_is_host) {
        const std::size_t authority_end = remainder.find_first_of("/?");
        if (authority_end == std::string_view::npos) {
            parsed.host = as_lower(remainder);
            parsed.path = "/";
            return parsed;
        }
        parsed.host = as_lower(remainder.substr(0, authority_end));
        remainder = remainder.substr(authority_end);
    }
    const std::size_t query_at = remainder.find('?');
    const std::string_view path_part =
        query_at == std::string_view::npos ? remainder : remainder.substr(0, query_at);
    if (query_at != std::string_view::npos) {
        parsed.query = remainder.substr(query_at + 1);
    }
    if (authority_is_host) {
        parsed.path = std::string(path_part);
    } else {
        // A custom scheme's `scheme://` is followed directly by the first route segment, so the
        // canonical spelling adds the separator the author omitted (`myapp://orders/42`).
        parsed.path = "/";
        parsed.path.append(path_part);
    }
    if (parsed.path.empty()) {
        parsed.path = "/";
    }
    return parsed;
}

} // namespace

std::expected<DeepLink, NavigationError> parse_deep_link(std::string_view text,
                                                         const DeepLinkConfig& config,
                                                         ActivationSource source, bool cold_start) {
    // Section 7: "Max URL length; reject malformed/oversized". One comparison, before any
    // allocation.
    if (text.size() > config.max_url_length) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::PathTooLong, {}, config.max_url_length});
    }
    if (text.empty()) {
        return std::unexpected(NavigationError{NavigationErrorCode::InvalidPath, {}, 0});
    }
    // A fragment is browser-only state and is never part of a route, so it is dropped rather than
    // decoded into a segment.
    const std::size_t fragment_at = text.find('#');
    if (fragment_at != std::string_view::npos) {
        text = text.substr(0, fragment_at);
    }

    // `http`/`https` are the host-based schemes (App/Universal Links, deep-linking-v1.md section
    // 1). Any other registered scheme is a custom scheme whose first segment is a route segment,
    // which is how everyone writes `myapp://orders/42`. The scheme is read off the front before the
    // split, because which shape applies is a property of the scheme, not of the text after it.
    std::string scheme;
    if (const std::size_t scheme_end = text.find("://"); scheme_end != std::string_view::npos) {
        scheme = as_lower(text.substr(0, scheme_end));
        if (!is_valid_scheme(scheme)) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnsupportedScheme, scheme, 0});
        }
        if (!allowlisted(config.schemes, scheme)) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnsupportedScheme, scheme, 0});
        }
    }
    const bool host_based = scheme == "http" || scheme == "https";
    const ParsedUrl parsed = split_url(text, host_based);
    if (!parsed.has_authority) {
        if (!config.accept_bare_path) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnsupportedScheme, std::string(text), 0});
        }
        if (parsed.path.empty() || parsed.path.front() != '/') {
            return std::unexpected(
                NavigationError{NavigationErrorCode::InvalidPath, std::string(text), 0});
        }
    } else if (host_based) {
        if (parsed.host.empty()) {
            return std::unexpected(NavigationError{NavigationErrorCode::UnsupportedHost, {}, 0});
        }
        // Section 3's `hosts` allowlist is load-bearing exactly for a host-based scheme, which is
        // what an App/Universal Link is. It must not apply to a custom scheme, whose "authority" is
        // a route segment: `myapp://orders/42` would otherwise be refused for an unlisted host
        // named `orders`.
        if (!allowlisted(config.hosts, parsed.host)) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::UnsupportedHost, parsed.host, 0});
        }
    }

    // Every segment is decoded once, by the same function a route match uses, so a link and a
    // direct push cannot disagree about what a segment means (section 7: encoded separators,
    // traversal).
    std::size_t start = 1;
    while (start <= parsed.path.size()) {
        const std::size_t end = parsed.path.find('/', start);
        const std::string_view segment = std::string_view{parsed.path}.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!segment.empty()) {
            auto decoded = percent_decode(segment);
            if (!decoded.has_value()) {
                NavigationError error = decoded.error();
                error.offset += start;
                return std::unexpected(error);
            }
            if (*decoded == "." || *decoded == "..") {
                return std::unexpected(
                    NavigationError{NavigationErrorCode::InvalidPath, *decoded, start});
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    DeepLink link;
    link.source = source;
    link.cold_start = cold_start;
    link.path = parsed.path;
    // The normalized URL is what a diagnostic and a dedupe key read: scheme and host lowercased,
    // the fragment gone, the path and query as given. A custom scheme has no host to write back, so
    // its normalized form is `<scheme>://` followed by the canonical path.
    if (parsed.has_authority) {
        link.url = host_based ? parsed.scheme + "://" + parsed.host + link.path
                              : parsed.scheme + "://" + link.path.substr(1);
    } else {
        link.url = link.path;
    }
    if (!parsed.query.empty()) {
        link.url += "?";
        link.url += parsed.query;
    }
    return link;
}

std::expected<DeepLink, NavigationError> resolve_deep_link(const RouteTable& table, DeepLink link) {
    auto match = table.match(link.path);
    if (!match.has_value()) {
        // Section 5: "Unknown route -> the `notFound` route ... never silently ignore." The failure
        // is reported and the caller routes it, so which screen "not found" is stays table data
        // (navigation-v1.md section 3).
        return std::unexpected(match.error());
    }
    link.route_id = match->route_id;
    link.params = std::move(match->params);
    link.rest = match->rest;
    return link;
}

std::expected<DeepLinkResult, NavigationError>
install_deep_link(const RouteTable& table, NavController& controller, const DeepLinkConfig& config,
                  std::string_view text, ActivationSource source, bool cold_start) {
    auto parsed = parse_deep_link(text, config, source, cold_start);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    auto resolved = resolve_deep_link(table, std::move(*parsed));
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }

    DeepLinkResult result;
    result.route_id = resolved->route_id;
    const RouteDef* route = table.find(resolved->route_id);
    result.requires_auth = route != nullptr && route->auth;

    // navigation-v1.md section 5's "Cold start: ... preserve the pending link" and
    // deep-linking-v1.md section 2's warm-start rule, without a queue: with an empty stack the
    // route root is seeded first so `back()` from a cold deep link has a destination instead of
    // reporting `Empty`. The seed is skipped when the link already targets the root, because
    // pushing the root under itself would make back-at-root a no-op twice over.
    const std::string_view root = table.initial_route();
    if (controller.depth() == 0) {
        result.outcome = DeepLinkOutcome::Synthesized;
        if (!root.empty() && resolved->route_id != root) {
            auto seeded = controller.push_route(root, {});
            if (!seeded.has_value()) {
                // The initial route was validated when the table was built, so this cannot fail for
                // a valid table; reporting it keeps a corrupt table from producing a half stack.
                return std::unexpected(seeded.error());
            }
        }
    } else {
        result.outcome = DeepLinkOutcome::Pushed;
    }

    auto pushed = controller.push_route(resolved->route_id, std::move(resolved->params));
    if (!pushed.has_value()) {
        return std::unexpected(pushed.error());
    }
    if (*pushed == PushOutcome::DuplicateIgnored) {
        result.outcome = DeepLinkOutcome::DuplicateIgnored;
    }
    return result;
}

} // namespace tessera::appfw
