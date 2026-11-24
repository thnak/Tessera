// SPDX-License-Identifier: MIT
//
// The WinHTTP error mapping and URL rules (issue #835).
//
// **Why these are testable everywhere.** Nothing here includes a Windows header, so the lane that
// runs the contract suite also pins the two decisions a WinHTTP back end makes that a reviewer can
// disagree with: which error kind a code becomes, and how a URL splits. A Windows-only test would
// leave the Linux lane unable to notice a mapping regression, and the mapping is the part section 3
// item 6 is about.

#include "tessera/appfw/net_winhttp_map.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::appfw::error_kind_for_windows_error;
using tessera::appfw::error_kind_for_winhttp_status;
using tessera::appfw::NetErrorKind;
using tessera::appfw::split_winhttp_url;
using tessera::appfw::WinHttpUrlParts;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

void test_each_distinct_kind_stays_distinct() {
    // Section 3 item 6's whole point: a caller retries `timeout`, drops `cancelled`, and surfaces
    // `dns`/`connect`/`tls` differently. Collapsing any two of these is the failure this pins.
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpNameNotResolved) ==
              NetErrorKind::Dns,
          "a name that did not resolve is dns");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpCannotConnect) ==
              NetErrorKind::Connect,
          "a connection that did not open is connect");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpConnectionError) ==
              NetErrorKind::Connect,
          "a broken connection is still connect");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpTimeout) == NetErrorKind::Timeout,
          "a platform timeout is timeout, never connect");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpSecureFailure) == NetErrorKind::Tls,
          "a certificate or handshake failure is tls");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpOperationCancelled) ==
              NetErrorKind::Cancelled,
          "a closed handle is cancelled");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpInvalidUrl) == NetErrorKind::BadUrl,
          "a URL WinHTTP refused is bad_url");
    check(error_kind_for_winhttp_status(tessera::appfw::kWinHttpUnrecognizedScheme) ==
              NetErrorKind::BadUrl,
          "and so is an unknown scheme");
}

void test_an_unclaimed_code_is_other() {
    // Honest rather than a guess: a code this mapping does not claim must not be presented as one
    // it understands, because the caller's retry policy branches on the kind.
    check(error_kind_for_winhttp_status(0) == NetErrorKind::Other, "success-as-error is other");
    check(error_kind_for_winhttp_status(999999) == NetErrorKind::Other, "an unknown code is other");
}

void test_the_socket_codes_map_too() {
    // WinHTTP passes some stack errors through; a resolver's "no such host" is `dns`, not `other`.
    check(error_kind_for_windows_error(11001) == NetErrorKind::Dns, "WSAHOST_NOT_FOUND is dns");
    check(error_kind_for_windows_error(11002) == NetErrorKind::Dns, "WSATRY_AGAIN is dns");
    check(error_kind_for_windows_error(10061) == NetErrorKind::Connect,
          "WSAECONNREFUSED is connect");
    check(error_kind_for_windows_error(10060) == NetErrorKind::Timeout, "WSAETIMEDOUT is timeout");
    check(error_kind_for_windows_error(10051) == NetErrorKind::Connect,
          "WSAENETUNREACH is connect");
    check(error_kind_for_windows_error(10065) == NetErrorKind::Connect,
          "WSAEHOSTUNREACH is connect");
    // A reset connection is neither: the connection had opened and nobody cancelled it.
    check(error_kind_for_windows_error(10054) == NetErrorKind::Other, "WSAECONNRESET is other");
}

void test_a_plain_url_splits() {
    WinHttpUrlParts parts;
    std::string why;
    check(split_winhttp_url("http://example.com/a/b?c=d", parts, why), why);
    check(parts.host == "example.com", "the host is the authority");
    check(parts.port == 80, "a plain http URL defaults to port 80");
    check(!parts.secure, "and is not secure");
    check(parts.target == "/a/b?c=d", "the target keeps the path and query");
}

void test_https_defaults_to_443() {
    WinHttpUrlParts parts;
    std::string why;
    check(split_winhttp_url("https://example.com", parts, why), why);
    check(parts.secure, "https is secure");
    check(parts.port == 443, "and defaults to 443");
    check(parts.target == "/", "a URL with no path becomes /, because an empty one is a different "
                               "request to WinHTTP");
}

void test_an_explicit_port_wins() {
    WinHttpUrlParts parts;
    std::string why;
    check(split_winhttp_url("http://127.0.0.1:8080/x", parts, why), why);
    check(parts.host == "127.0.0.1", "the host has no port in it");
    check(parts.port == 8080, "the explicit port is used");
    check(parts.target == "/x", "and the target is the path");
}

void test_an_ipv6_literal_keeps_its_brackets_apart_from_the_port() {
    // The colon search must start after the bracket, or `[::1]:80` would split at the first colon
    // inside the literal and name a host of "[".
    WinHttpUrlParts parts;
    std::string why;
    check(split_winhttp_url("http://[::1]:9000/p", parts, why), why);
    check(parts.host == "[::1]", "the bracketed literal is the host");
    check(parts.port == 9000, "and the port after the bracket is found");
}

void test_a_query_without_a_path_is_a_target() {
    WinHttpUrlParts parts;
    std::string why;
    check(split_winhttp_url("http://h?a=b", parts, why), why);
    check(parts.target == "?a=b", "the query is the target when there is no path");
}

void test_refusals_name_their_reason() {
    WinHttpUrlParts parts;
    std::string why;
    // Each of these is a URL a caller could pass and would otherwise get a confusing WinHTTP error
    // for. The reason is what the diagnostic says, so it must not be empty.
    const char* refused[] = {
        "example.com/a",                 // no scheme
        "ftp://example.com/",            // not http(s)
        "http://",                       // no host
        "http://example.com/#frag",      // a fragment is not sent
        "http://user:pass@example.com/", // userinfo is a header's job
        "http://example.com:0/",         // port 0
        "http://example.com:99999/",     // port out of range
        "http://example.com:abc/",       // port is not a number
        "http://[::1/",                  // unterminated IPv6 literal
    };
    for (const char* url : refused) {
        why.clear();
        const bool ok = split_winhttp_url(url, parts, why);
        check(!ok, std::string("refused: ") + url);
        check(!why.empty(), std::string("with a reason: ") + url);
    }
}

} // namespace

int main() {
    test_each_distinct_kind_stays_distinct();
    test_an_unclaimed_code_is_other();
    test_the_socket_codes_map_too();
    test_a_plain_url_splits();
    test_https_defaults_to_443();
    test_an_explicit_port_wins();
    test_an_ipv6_literal_keeps_its_brackets_apart_from_the_port();
    test_a_query_without_a_path_is_a_target();
    test_refusals_name_their_reason();
    if (failures != 0) {
        std::cerr << "net_winhttp_map: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "net_winhttp_map: ok\n";
    return 0;
}
