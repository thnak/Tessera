// SPDX-License-Identifier: MIT
//
// The libcurl error table (issue #836). No curl header: see `net_curl_map.hpp`.

#include "tessera/appfw/net_curl_map.hpp"

namespace tessera::appfw {

NetErrorKind error_kind_for_curl_code(std::int32_t code) noexcept {
    switch (code) {
    case kCurlCouldntResolveHost:
    case kCurlCouldntResolveProxy:
        return NetErrorKind::Dns;
    case kCurlCouldntConnect:
        return NetErrorKind::Connect;
    case kCurlOperationTimedout:
        return NetErrorKind::Timeout;
    case kCurlSslConnectError:
    case kCurlPeerFailedVerification:
    case kCurlSslCertproblem:
    case kCurlSslCipher:
    case kCurlSslCacertBadfile:
    case kCurlSslIssuerError:
        return NetErrorKind::Tls;
    case kCurlUrlMalformat:
    case kCurlUnsupportedProtocol:
        return NetErrorKind::BadUrl;
    case kCurlAbortedByCallback:
        return NetErrorKind::Cancelled;
    default:
        return NetErrorKind::Other;
    }
}

} // namespace tessera::appfw
