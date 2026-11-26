// SPDX-License-Identifier: MIT
//
// The libcurl error table (issue #836). Nothing here includes curl.h, so every lane pins the part a
// reviewer can disagree with: which `NetErrorKind` a `CURLcode` becomes. The curl translation unit
// asserts the constants against the real header, so a transcription mistake cannot survive a build
// that has libcurl.

#include "tessera/appfw/net_curl_map.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::appfw::error_kind_for_curl_code;
using tessera::appfw::NetErrorKind;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

void test_the_distinct_kinds_stay_distinct() {
    // Section 3 item 6: a caller retries `timeout`, drops `cancelled`, and surfaces
    // `dns`/`connect`/`tls` differently. Collapsing any two of these is what this pins.
    check(error_kind_for_curl_code(tessera::appfw::kCurlCouldntResolveHost) == NetErrorKind::Dns,
          "an unresolved host is dns");
    check(error_kind_for_curl_code(tessera::appfw::kCurlCouldntResolveProxy) == NetErrorKind::Dns,
          "an unresolved proxy is dns too");
    check(error_kind_for_curl_code(tessera::appfw::kCurlCouldntConnect) == NetErrorKind::Connect,
          "a connection that did not open is connect");
    check(error_kind_for_curl_code(tessera::appfw::kCurlOperationTimedout) == NetErrorKind::Timeout,
          "a platform timeout is timeout, never connect");
    check(error_kind_for_curl_code(tessera::appfw::kCurlSslConnectError) == NetErrorKind::Tls,
          "a handshake failure is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlPeerFailedVerification) ==
              NetErrorKind::Tls,
          "a certificate that did not verify is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlSslCertproblem) == NetErrorKind::Tls,
          "a client certificate problem is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlSslCipher) == NetErrorKind::Tls,
          "a cipher failure is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlSslIssuerError) == NetErrorKind::Tls,
          "an issuer error is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlSslCacertBadfile) == NetErrorKind::Tls,
          "a bad CA bundle is tls");
    check(error_kind_for_curl_code(tessera::appfw::kCurlUrlMalformat) == NetErrorKind::BadUrl,
          "a malformed URL is bad_url");
    check(error_kind_for_curl_code(tessera::appfw::kCurlUnsupportedProtocol) ==
              NetErrorKind::BadUrl,
          "an unsupported scheme is bad_url");
    check(error_kind_for_curl_code(tessera::appfw::kCurlAbortedByCallback) ==
              NetErrorKind::Cancelled,
          "an aborted transfer is cancelled");
}

void test_an_unclaimed_code_is_other() {
    check(error_kind_for_curl_code(0) == NetErrorKind::Other, "CURLE_OK is not claimed");
    check(error_kind_for_curl_code(tessera::appfw::kCurlWriteError) == NetErrorKind::Other,
          "a write error is other: the transport maps its own too_large first");
    check(error_kind_for_curl_code(56) == NetErrorKind::Other, "a receive error is other");
    check(error_kind_for_curl_code(-1) == NetErrorKind::Other, "a negative code is other");
}

} // namespace

int main() {
    test_the_distinct_kinds_stay_distinct();
    test_an_unclaimed_code_is_other();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
