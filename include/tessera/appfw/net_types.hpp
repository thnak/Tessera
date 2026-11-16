// SPDX-License-Identifier: MIT
//
// The `net` module's value types (docs/specs/networking-v1.md section 2; ADR-052 item 2; issue
// #197). This is the interface an app sees: a request, a buffered response and a named error. No
// socket, no platform handle and no back-end type appears here, which is ADR-052's "one
// `fetch`-shaped interface with no platform types" stated as a header.
//
// Names: the spec's table calls these `Request`, `Response` and `NetError`. Two of the three are
// spelled `NetRequest`/`NetResponse` here so an app-framework header does not put a bare
// `Request`/`Response` into `tessera::appfw` - the same reason AF1 spells the ADR's `Status` as
// `ServiceStatus`. `NetError` keeps the spec's name because it is already qualified.
//
// What section 2 fixes, and what this header therefore carries:
//
//   * `method` is one of the six the spec lists, `url` is an absolute http/https string,
//     `headers` is an ordered name/value list, `body` is optional bytes, `timeout` is per-attempt
//     in milliseconds (default 30000), `max_response_bytes` defaults to 16 MiB, and `redirect` is
//     `follow` or `manual`.
//   * `status` is an `int` and **a 4xx/5xx response is not an error at this layer**: it is a
//     `NetResponse` and the caller decides (async-data-v1.md maps statuses to retry rules).
//   * `NetError.kind` is exactly the spec's eleven kinds, in the spec's order, so an added kind is
//     visible as a diff against section 6's "adding an error kind is a minor change" rule.
//   * `protocol` reports h1/h2/h3/unknown; nothing in V1 asks for a version, the platform stack
//     picks it (section 7's third open question).
//
// Deliberately absent, and named in docs/design/networking-module.md section 7: streamed bodies
// (section 2's `Response` row mentions `Request.stream` but the `Request` row never declares it),
// WebSocket and streaming upload (section 1 non-goals), certificate pinning, and any cookie jar or
// HTTP cache. A body is one buffered buffer in both directions.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// Section 2's method set.
enum class HttpMethod : std::uint8_t { Get = 0, Head, Post, Put, Patch, Delete };

[[nodiscard]] constexpr std::string_view http_method_name(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Head:
        return "HEAD";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Patch:
        return "PATCH";
    case HttpMethod::Delete:
        return "DELETE";
    }
    return "GET";
}

// RFC 9110's idempotent methods, restricted to what a caller's retry policy needs. `net` itself
// never retries: section 3 item 3 says "There is no implicit retry", and async-data-v1.md section 3
// owns the backoff policy. This predicate is the fact that policy branches on.
[[nodiscard]] constexpr bool http_method_is_idempotent(HttpMethod method) noexcept {
    return method == HttpMethod::Get || method == HttpMethod::Head || method == HttpMethod::Put ||
           method == HttpMethod::Delete;
}

// Section 2's `redirect`. `Follow` applies section 3 item 2's rules; `Manual` hands a 3xx back as a
// `NetResponse` and the caller decides.
enum class RedirectPolicy : std::uint8_t { Follow = 0, Manual };

[[nodiscard]] constexpr std::string_view redirect_policy_name(RedirectPolicy policy) noexcept {
    return policy == RedirectPolicy::Follow ? "follow" : "manual";
}

// Section 2's `protocol`. `Unknown` is what a back end reports when the platform does not expose
// the negotiated version; it is not an error.
enum class NetProtocol : std::uint8_t { Unknown = 0, H1, H2, H3 };

[[nodiscard]] constexpr std::string_view net_protocol_name(NetProtocol protocol) noexcept {
    switch (protocol) {
    case NetProtocol::Unknown:
        return "unknown";
    case NetProtocol::H1:
        return "h1";
    case NetProtocol::H2:
        return "h2";
    case NetProtocol::H3:
        return "h3";
    }
    return "unknown";
}

// Section 2's `NetError.kind`, verbatim and in the spec's order. The distinctions that matter:
// `timeout`, `cancelled` and a transport failure (`dns`/`connect`/`tls`/`offline`) are separate
// outcomes because a caller retries the first, drops the second and surfaces the third.
// `unsupported` is section 4's "a back end may be unavailable in a build", and also this module's
// refusal of cleartext `http` when `net.allowCleartext` is off (section 3 item 8).
enum class NetErrorKind : std::uint8_t {
    Cancelled = 0,
    Timeout,
    Offline,
    Dns,
    Connect,
    Tls,
    TooLarge,
    TooManyRedirects,
    BadUrl,
    Unsupported,
    Other,
};

[[nodiscard]] constexpr std::string_view net_error_kind_name(NetErrorKind kind) noexcept {
    switch (kind) {
    case NetErrorKind::Cancelled:
        return "cancelled";
    case NetErrorKind::Timeout:
        return "timeout";
    case NetErrorKind::Offline:
        return "offline";
    case NetErrorKind::Dns:
        return "dns";
    case NetErrorKind::Connect:
        return "connect";
    case NetErrorKind::Tls:
        return "tls";
    case NetErrorKind::TooLarge:
        return "too_large";
    case NetErrorKind::TooManyRedirects:
        return "too_many_redirects";
    case NetErrorKind::BadUrl:
        return "bad_url";
    case NetErrorKind::Unsupported:
        return "unsupported";
    case NetErrorKind::Other:
        return "other";
    }
    return "other";
}

// Section 2's `NetError`. `os_code` is `0` for "none": the spec calls it optional and diagnostic
// only, so no rule here branches on it and a plain integer keeps the struct copyable and
// comparison-friendly.
struct NetError {
    NetErrorKind kind = NetErrorKind::Other;
    std::string message;
    std::int64_t os_code = 0;

    [[nodiscard]] bool operator==(const NetError&) const = default;
};

// Section 2's `Response`. `body` is empty for `HEAD` and for `204`; `final_url` differs from
// `NetRequest::url` after a followed redirect.
struct NetResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string final_url;
    NetProtocol protocol = NetProtocol::Unknown;

    [[nodiscard]] bool operator==(const NetResponse&) const = default;
};

// The ordered name/value list of section 2's `headers` field. It is a `vector` of pairs so the send
// order is the caller's order and a repeated name is preserved (sending two `Accept` values is
// legal); every *lookup* is case-insensitive, which is section 3 item 7's "names compare
// case-insensitively and are stored as sent".
using NetHeaders = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] bool header_name_equals(std::string_view left, std::string_view right) noexcept;

// The first value under `name`, or null. Case-insensitive on the name only.
[[nodiscard]] const std::string* find_header(const NetHeaders& headers,
                                             std::string_view name) noexcept;

[[nodiscard]] bool has_header(const NetHeaders& headers, std::string_view name) noexcept;

// Replaces the first value under `name` in place (so a caller's send order survives) and drops any
// duplicate names. Appends when absent.
void set_header(NetHeaders& headers, std::string_view name, std::string value);

// Removes every entry under `name`; returns how many were removed.
std::size_t remove_header(NetHeaders& headers, std::string_view name);

// Section 2's `Request`, with the spec's defaults: a GET with a 30 s per-attempt timeout, a 16 MiB
// response cap and `follow`.
struct NetRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    NetHeaders headers;
    // A body is optional, and an empty body on `POST` is legal, so presence is its own flag rather
    // than `body.empty()`.
    std::string body;
    bool has_body = false;
    // Per attempt, in milliseconds, measured from the start of the attempt to the last body byte
    // (section 3 item 3). `0` disables the deadline.
    std::uint32_t timeout_ms = 30000;
    // `0` disables the cap. Section 3 item 5: a larger body is `too_large` and the bytes read so
    // far are discarded.
    std::size_t max_response_bytes = 16u * 1024u * 1024u;
    RedirectPolicy redirect = RedirectPolicy::Follow;
};

// Section 2's `Response`-or-error. A 4xx/5xx status is a value here, never an error.
using NetResult = std::expected<NetResponse, NetError>;

// Section 3 item 2's hop count: the eleventh redirect is `too_many_redirects`.
inline constexpr std::uint32_t kMaxRedirects = 10;

// "a fixed `Tessera/<version>`" (section 3 item 7). Empty means "the module's default", so a caller
// that wants no `User-Agent` at all sets it to a single space and a caller that wants its own sets
// it explicitly; the module never invents a second one.
[[nodiscard]] std::string default_user_agent();

} // namespace tessera::appfw
