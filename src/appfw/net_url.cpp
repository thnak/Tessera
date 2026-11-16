// SPDX-License-Identifier: MIT
//
// URL parsing and redaction for the `net` module (docs/specs/networking-v1.md sections 2, 3 items
// 2, 8 and 9; issue #197). One pass over the string, no allocation on the failure path beyond the
// diagnostic message, and every rejection names the rule that did not hold.

#include "tessera/appfw/net_url.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::appfw {

namespace {

constexpr char ascii_lower(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

// A URL may not carry a control character or a raw space: those are the two shapes that let a value
// inject a header line into a platform stack, so they are refused here rather than downstream.
[[nodiscard]] bool has_forbidden_character(std::string_view text) noexcept {
    for (const char value : text) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] NetError bad_url(std::string message) {
    return NetError{NetErrorKind::BadUrl, std::move(message), 0};
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string result(text);
    for (char& value : result) {
        value = ascii_lower(value);
    }
    return result;
}

// The port's default for a scheme, or 0 for a scheme that has none.
[[nodiscard]] std::uint16_t default_port(std::string_view scheme) noexcept {
    if (scheme == "https") {
        return kHttpsDefaultPort;
    }
    if (scheme == "http") {
        return kHttpDefaultPort;
    }
    return 0;
}

} // namespace

std::expected<NetUrl, NetError> parse_url(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(bad_url("an empty URL"));
    }
    if (text.size() > kMaxUrlLength) {
        return std::unexpected(bad_url("a URL longer than the limit"));
    }
    if (has_forbidden_character(text)) {
        return std::unexpected(bad_url("a URL with a control character or a space"));
    }

    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::unexpected(bad_url("a URL without an absolute scheme"));
    }
    const NetUrl candidate_scheme{lowercase(text.substr(0, scheme_end)), {}, 0, {}, {}, {}};
    if (candidate_scheme.scheme.empty()) {
        return std::unexpected(bad_url("a URL without a scheme"));
    }
    // A syntactically valid URL whose scheme this module does not implement is `unsupported`, not
    // `bad_url`: the distinction is what lets a caller tell a typo from a transport the build
    // lacks.
    if (default_port(candidate_scheme.scheme) == 0) {
        return std::unexpected(
            NetError{NetErrorKind::Unsupported, "scheme is not http or https", 0});
    }

    std::string_view rest = text.substr(scheme_end + 3);
    const std::size_t fragment = rest.find('#');
    if (fragment != std::string_view::npos) {
        rest = rest.substr(0, fragment);
    }
    const std::size_t authority_end = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, authority_end);
    rest =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    NetUrl url;
    url.scheme = candidate_scheme.scheme;

    // userinfo is everything before the *last* '@' in the authority: a password may contain one.
    std::string_view host_port = authority;
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        url.userinfo = std::string(authority.substr(0, at));
        host_port = authority.substr(at + 1);
    }
    if (host_port.empty()) {
        return std::unexpected(bad_url("a URL without a host"));
    }

    if (host_port.front() == '[') {
        const std::size_t close = host_port.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(bad_url("an IPv6 literal without its closing bracket"));
        }
        url.host = lowercase(host_port.substr(0, close + 1));
        host_port = host_port.substr(close + 1);
        if (!host_port.empty() && host_port.front() != ':') {
            return std::unexpected(bad_url("an IPv6 literal followed by a port without ':'"));
        }
    } else {
        const std::size_t colon = host_port.find(':');
        url.host = lowercase(host_port.substr(0, colon));
        host_port = colon == std::string_view::npos ? std::string_view{} : host_port.substr(colon);
    }
    if (url.host.empty()) {
        return std::unexpected(bad_url("a URL without a host"));
    }

    url.port = default_port(url.scheme);
    if (!host_port.empty()) {
        if (host_port.front() != ':' || host_port.size() == 1) {
            return std::unexpected(bad_url("a port without a value"));
        }
        std::uint32_t value = 0;
        for (const char digit : host_port.substr(1)) {
            if (!is_digit(digit)) {
                return std::unexpected(bad_url("a non-numeric port"));
            }
            value = value * 10u + static_cast<std::uint32_t>(digit - '0');
            if (value > 65535u) {
                return std::unexpected(bad_url("a port above 65535"));
            }
        }
        if (value == 0) {
            return std::unexpected(bad_url("port 0"));
        }
        url.port = static_cast<std::uint16_t>(value);
    }

    if (const std::size_t query = rest.find('?'); query != std::string_view::npos) {
        url.query = std::string(rest.substr(query + 1));
        rest = rest.substr(0, query);
    }
    if (rest.empty()) {
        url.path = "/";
    } else if (rest.front() == '/') {
        url.path = std::string(rest);
    } else {
        return std::unexpected(bad_url("a path that does not start with '/'"));
    }
    return url;
}

std::string url_string(const NetUrl& url) {
    std::string result;
    result.reserve(url.scheme.size() + url.host.size() + url.path.size() + url.query.size() + 24);
    result += url.scheme;
    result += "://";
    if (!url.userinfo.empty()) {
        result += url.userinfo;
        result += '@';
    }
    result += url.host;
    // A default port is omitted so a URL that never spelled one round-trips unchanged; a
    // non-default port is printed because dropping it would name a different origin.
    if (url.port != default_port(url.scheme)) {
        result += ':';
        result += std::to_string(url.port);
    }
    result += url.path;
    if (!url.query.empty()) {
        result += '?';
        result += url.query;
    }
    return result;
}

std::string redacted_url(std::string_view text) {
    const auto parsed = parse_url(text);
    if (parsed.has_value()) {
        std::string result = parsed->scheme;
        result += "://";
        result += parsed->host;
        if (parsed->port != default_port(parsed->scheme)) {
            result += ':';
            result += std::to_string(parsed->port);
        }
        result += parsed->path;
        return result;
    }
    // An unparsable URL still may not leak a query string or a userinfo, so the redaction is
    // applied to the raw text: cut at the first '?' or '#', then drop everything before the last
    // '@' of the authority.
    std::string_view kept = text;
    if (const std::size_t cut = kept.find_first_of("?#"); cut != std::string_view::npos) {
        kept = kept.substr(0, cut);
    }
    if (const std::size_t scheme = kept.find("://"); scheme != std::string_view::npos) {
        const std::string_view prefix = kept.substr(0, scheme + 3);
        const std::string_view remainder = kept.substr(scheme + 3);
        const std::size_t slash = remainder.find('/');
        const std::string_view authority = remainder.substr(0, slash);
        if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
            std::string result(prefix);
            result += authority.substr(at + 1);
            if (slash != std::string_view::npos) {
                result += remainder.substr(slash);
            }
            return result;
        }
    }
    if (kept.size() > kMaxUrlLength) {
        kept = kept.substr(0, kMaxUrlLength);
    }
    return std::string(kept);
}

NetOrigin origin_of(const NetUrl& url) {
    return NetOrigin{url.scheme, url.host, url.port};
}

bool is_cleartext_scheme(const NetUrl& url) noexcept {
    return url.scheme == "http";
}

} // namespace tessera::appfw
