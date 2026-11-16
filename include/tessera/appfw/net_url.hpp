// SPDX-License-Identifier: MIT
//
// URLs for the `net` module (docs/specs/networking-v1.md sections 2, 3 items 2, 8 and 9; ADR-052;
// issue #197).
//
// Why the module parses a URL at all, when every back end parses one too: three normative rules
// need a structural answer rather than a substring check, and the contract suite asserts all three
// against the fake back end.
//
//   1. **`bad_url` is a named outcome** (section 2's error list), so a URL that is not an absolute
//      http/https URL is refused before a byte is read instead of being discovered by a platform
//      stack and reported as `other`.
//   2. **A cross-origin redirect drops `Authorization`** (section 3 item 2). That is an origin
//      comparison - scheme, host and effective port - not a string comparison:
//      `https://a.example/x` and `https://a.example:443/y` are one origin, `http://a.example/y` is
//      another.
//   3. **Diagnostics redact the query string and the userinfo** (section 3 item 9), so those two
//      parts have to be separable from the rest.
//
// Deliberately not implemented, and named in docs/design/networking-module.md section 7:
// percent-encoding normalisation, IDNA/punycode, and relative-reference resolution (`../`, `?q`).
// A redirect `Location` must therefore be absolute; a relative one is refused as `unsupported`.
// Every shipped back end resolves references inside its own stack, so this is a gap in the
// fake/contract path rather than in the shipped behaviour.

#pragma once

#include "tessera/appfw/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::appfw {

inline constexpr std::uint16_t kHttpDefaultPort = 80;
inline constexpr std::uint16_t kHttpsDefaultPort = 443;
// Bound on untrusted input: a longer URL is `bad_url`, so no diagnostic carries an unbounded string
// and no parser is fed an arbitrarily large one.
inline constexpr std::size_t kMaxUrlLength = 8192;

// `scheme://[userinfo@]host[:port]/path[?query][#fragment]`. `port` always holds the *effective*
// port (the scheme's default when the text omits it), so two spellings of one origin compare equal.
// The fragment is parsed and dropped: it is never sent, and keeping it would make `final_url`
// disagree with the wire.
struct NetUrl {
    std::string scheme; // lowercase, "http" or "https"
    std::string host;   // lowercase; an IPv6 literal keeps its brackets
    std::uint16_t port = 0;
    std::string path;     // always starts with '/', at least "/"
    std::string query;    // without the leading '?', empty when absent
    std::string userinfo; // without the trailing '@'; never used for authentication

    [[nodiscard]] bool operator==(const NetUrl&) const = default;
};

// scheme + host + effective port: the tuple section 3 item 2's `Authorization` rule compares.
struct NetOrigin {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;

    [[nodiscard]] bool operator==(const NetOrigin&) const = default;
};

// Parses an absolute http/https URL. Every failure is `NetErrorKind::BadUrl` with a message naming
// the rule that did not hold; the message never repeats the query string or the userinfo
// (section 3 item 9). `NetErrorKind::Unsupported` is returned for a syntactically valid URL whose
// cleartext scheme is refused - the caller decides that, so it is not returned here.
[[nodiscard]] std::expected<NetUrl, NetError> parse_url(std::string_view text);

// The full URL as it would be sent, userinfo and query included. Used for `NetResponse::final_url`
// and for the transport's request, never for a diagnostic.
[[nodiscard]] std::string url_string(const NetUrl& url);

// The URL a diagnostic may print: scheme, host, port and path only (section 3 item 9). It is
// applied to text that may not parse, so a malformed URL cannot smuggle its query string into a log
// line either.
[[nodiscard]] std::string redacted_url(std::string_view text);

[[nodiscard]] NetOrigin origin_of(const NetUrl& url);

// True for an `http` URL, which section 3 item 8 permits only behind `net.allowCleartext`.
[[nodiscard]] bool is_cleartext_scheme(const NetUrl& url) noexcept;

} // namespace tessera::appfw
