// SPDX-License-Identifier: MIT
//
// The WinHTTP mapping and URL rules (issue #835). No Windows header is included: see
// `net_winhttp_map.hpp` for why, and for the compile-time cross-check the Windows translation unit
// does against the real header.

#include "tessera/appfw/net_winhttp_map.hpp"

#include <string_view>

namespace tessera::appfw {

NetErrorKind error_kind_for_winhttp_status(std::uint32_t status) noexcept {
    switch (status) {
    case kWinHttpNameNotResolved:
        return NetErrorKind::Dns;
    case kWinHttpCannotConnect:
    case kWinHttpConnectionError:
        return NetErrorKind::Connect;
    case kWinHttpTimeout:
        return NetErrorKind::Timeout;
    case kWinHttpSecureFailure:
        return NetErrorKind::Tls;
    case kWinHttpInvalidUrl:
    case kWinHttpUnrecognizedScheme:
        return NetErrorKind::BadUrl;
    case kWinHttpOperationCancelled:
        return NetErrorKind::Cancelled;
    default:
        return NetErrorKind::Other;
    }
}

NetErrorKind error_kind_for_windows_error(std::uint32_t code) noexcept {
    switch (code) {
    // `WSAHOST_NOT_FOUND` and the friends a resolver reports when the name does not exist.
    case 11001:
    case 11002:
    case 11003:
        return NetErrorKind::Dns;
    // `WSAECONNREFUSED`, `WSAETIMEDOUT`, `WSAENETUNREACH`, `WSAEHOSTUNREACH`.
    case 10061:
        return NetErrorKind::Connect;
    case 10060:
        return NetErrorKind::Timeout;
    case 10051:
    case 10065:
        return NetErrorKind::Connect;
    // `WSAECONNRESET` and `WSAECONNABORTED`: the peer went away mid-connection. Not `cancelled` -
    // the caller did not ask for it - and not `connect` either, because the connection had opened.
    case 10054:
    case 10053:
        return NetErrorKind::Other;
    default:
        return NetErrorKind::Other;
    }
}

namespace {

bool is_scheme_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '-' || c == '.';
}

} // namespace

bool split_winhttp_url(std::string_view url, WinHttpUrlParts& out, std::string& why) {
    out = WinHttpUrlParts{};
    why.clear();

    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        why = "not an absolute URL";
        return false;
    }
    const std::string_view scheme = url.substr(0, scheme_end);
    for (const char c : scheme) {
        if (!is_scheme_char(c)) {
            why = "the scheme has an illegal character";
            return false;
        }
    }
    bool secure = false;
    if (scheme == "https" || scheme == "HTTPS") {
        secure = true;
    } else if (scheme != "http" && scheme != "HTTP") {
        // Section 3 item 8's cleartext refusal is the module's rule; this only says "not a scheme
        // this back end speaks".
        why = "the scheme is not http or https";
        return false;
    }
    out.secure = secure;

    std::string_view rest = url.substr(scheme_end + 3);
    // A fragment is not sent, so a URL with one is refused rather than silently truncated: the
    // caller asked for something this back end would not do.
    if (rest.find('#') != std::string_view::npos) {
        why = "a URL with a fragment is not a request target";
        return false;
    }

    std::string_view authority = rest;
    const std::size_t path_start = rest.find_first_of("/?");
    if (path_start != std::string_view::npos) {
        authority = rest.substr(0, path_start);
        out.target = std::string(rest.substr(path_start));
    } else {
        out.target = "/";
    }

    // Userinfo is allowed by the URL grammar and is NOT part of the host. It is refused here rather
    // than stripped: a `user:pass@` in a request URL is a credential the module's redaction rules
    // (#402) must not let reach a header, so the caller builds the header itself.
    if (authority.find('@') != std::string_view::npos) {
        why = "userinfo in the URL is not accepted; set the Authorization header";
        return false;
    }

    std::string_view host = authority;
    std::string_view port_text;
    // An IPv6 literal is bracketed, and the brackets may contain colons, so the port search starts
    // after the closing bracket.
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        if (close == std::string_view::npos) {
            why = "an IPv6 literal is missing its closing bracket";
            return false;
        }
        const std::size_t after = host.find(':', close);
        if (after != std::string_view::npos) {
            port_text = host.substr(after + 1);
            host = host.substr(0, after);
        }
    } else {
        const std::size_t colon = host.find(':');
        if (colon != std::string_view::npos) {
            port_text = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
    }
    if (host.empty()) {
        why = "the URL names no host";
        return false;
    }
    out.host = std::string(host);

    if (port_text.empty()) {
        out.port = secure ? 443 : 80;
        return true;
    }
    unsigned value = 0;
    for (const char c : port_text) {
        if (c < '0' || c > '9') {
            why = "the port is not a number";
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
        if (value > 65535) {
            why = "the port is out of range";
            return false;
        }
    }
    if (value == 0) {
        why = "port 0 is not a request target";
        return false;
    }
    out.port = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace tessera::appfw
