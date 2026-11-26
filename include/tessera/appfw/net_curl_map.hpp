// SPDX-License-Identifier: MIT
//
// The libcurl back end's error mapping (ADR-052 item 1, issue #836).
//
// **Why this part is separate from the transport.** The transport is a callback loop that only
// exists where libcurl is installed, but the decision a contract test can disagree with is a table:
// which `NetErrorKind` a `CURLcode` becomes. Section 3 item 6 says dns/connect/tls/timeout stay
// distinct, and that is testable on every lane. Like `net_winhttp_map.hpp`, the codes are plain
// integers (curl.h is not needed to test the table), and the curl translation unit asserts them
// against the real header at compile time.
#pragma once

#include "tessera/appfw/net_types.hpp"

#include <cstdint>

namespace tessera::appfw {

// The `CURLcode` values this mapping names.
inline constexpr std::int32_t kCurlUnsupportedProtocol = 1;
inline constexpr std::int32_t kCurlUrlMalformat = 3;
inline constexpr std::int32_t kCurlCouldntResolveProxy = 5;
inline constexpr std::int32_t kCurlCouldntResolveHost = 6;
inline constexpr std::int32_t kCurlCouldntConnect = 7;
inline constexpr std::int32_t kCurlWriteError = 23;
inline constexpr std::int32_t kCurlOperationTimedout = 28;
inline constexpr std::int32_t kCurlSslConnectError = 35;
inline constexpr std::int32_t kCurlAbortedByCallback = 42;
inline constexpr std::int32_t kCurlPeerFailedVerification = 60;
inline constexpr std::int32_t kCurlSslCertproblem = 58;
inline constexpr std::int32_t kCurlSslCipher = 59;
inline constexpr std::int32_t kCurlSslCacertBadfile = 77;
inline constexpr std::int32_t kCurlSslIssuerError = 83;

// The kind a `CURLcode` becomes. Anything not claimed is `other`, which is honest: a kind this
// table does not name is not pretended to mean something.
//   dns        the name did not resolve (host, or proxy).
//   connect    the name resolved and the connection did not open.
//   tls        the connection opened and the handshake or the certificate failed; a caller must not
//              retry it the way it retries connect.
//   timeout    `CURLE_OPERATION_TIMEDOUT`.
//   bad_url    refused before any I/O.
//   cancelled  the callback aborted the transfer (the transport maps its own causes first).
[[nodiscard]] NetErrorKind error_kind_for_curl_code(std::int32_t code) noexcept;

} // namespace tessera::appfw
