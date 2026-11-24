// SPDX-License-Identifier: MIT
//
// The WinHTTP back end's error mapping and URL splitting (ADR-052 item 1, issue #835).
//
// **Why this part is separate from the transport itself.** The transport is a callback state
// machine that only exists inside `#ifdef _WIN32`, but the two decisions it makes that a test can
// disagree with are pure: which `NetErrorKind` a WinHTTP status code becomes, and how a URL splits
// into the pieces WinHTTP is opened with. Section 3 item 6 says
// `offline`/`dns`/`connect`/`tls`/`timeout` stay distinct - that is a table, not a socket, so it is
// testable on every lane and the WinHTTP error codes are the only Windows-specific thing about it.
//
// Keeping them here also means the mapping can be reviewed against `docs/specs/networking-v1.md`
// section 3 without reading a callback chain.
#pragma once

#include "tessera/appfw/net_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::appfw {

// The WinHTTP error codes this mapping names, as plain integers.
//
// **Why not `#include <winhttp.h>`.** That header needs `<windows.h>` first and defines macros that
// break `std::min`, so a test on a Linux lane could not include this module at all. The values are
// documented constants from Microsoft's WInHTTP error list and are asserted against the real header
// by a compile-time check in the Windows translation unit, so a transcription mistake cannot
// survive a Windows build.
inline constexpr std::uint32_t kWinHttpNameNotResolved = 12007;
inline constexpr std::uint32_t kWinHttpCannotConnect = 12029;
inline constexpr std::uint32_t kWinHttpTimeout = 12002;
inline constexpr std::uint32_t kWinHttpSecureFailure = 12175;
inline constexpr std::uint32_t kWinHttpInvalidUrl = 12005;
inline constexpr std::uint32_t kWinHttpOperationCancelled = 12017;
inline constexpr std::uint32_t kWinHttpConnectionError = 12030;
inline constexpr std::uint32_t kWinHttpUnrecognizedScheme = 12006;

// The `NetErrorKind` a WinHTTP status/error code becomes.
//
// The distinctions the spec requires, and how WinHTTP expresses them:
//   - `dns`           the name did not resolve (`ERROR_WINHTTP_NAME_NOT_RESOLVED`).
//   - `connect`       the name resolved and the connection did not open.
//   - `tls`           the connection opened and the handshake or certificate failed. A distinct
//   kind
//                     because a caller must not retry it the way it retries `connect`.
//   - `timeout`       reported as `timeout`, never as `connect` - the caller retries this one
//                     differently (section 3 item 3).
//   - `cancelled`     the token closed the handle.
//   - `bad_url`       WinHTTP refused the URL before any I/O.
//
// Everything else is `other`: a kind this mapping does not claim, which is honest rather than
// pretending a code means something it does not.
[[nodiscard]] NetErrorKind error_kind_for_winhttp_status(std::uint32_t status) noexcept;

// The error kind a Windows socket/Win32 code means, for the codes WinHTTP passes through from the
// stack underneath. `kWinHttpCannotConnect` covers most of these, so this exists for the ones it
// does not.
[[nodiscard]] NetErrorKind error_kind_for_windows_error(std::uint32_t code) noexcept;

// A URL split into what an async WinHTTP open needs.
struct WinHttpUrlParts {
    // The host, without a scheme or path. ASCII/IDN as it appeared in the URL.
    std::string host;
    // The port, resolved from the scheme when the URL did not name one (80 or 443).
    std::uint16_t port = 0;
    // The path and query, always beginning with `/`. A URL with no path becomes `/`, because a
    // WinHTTP open with an empty path is a different request.
    std::string target;
    bool secure = false;
};

// Splits `url`, or returns an empty optional and sets `why` when it is not a usable absolute
// HTTP(S) URL.
//
// **Why a hand-written split and not `WinHttpCrackUrl`.** That function is Windows-only, so the
// parsing rules could not be tested on the lane that runs the contract suite's other halves, and
// its behaviour on an unusual URL (a userinfo section, an IPv6 literal, a trailing `?`) would then
// be whatever WinHTTP happened to do rather than something this project decided. The rules here are
// the ones `net_url.cpp` already implements for the module: absolute, `http` or `https`, a
// non-empty host, and no fragment.
[[nodiscard]] bool split_winhttp_url(std::string_view url, WinHttpUrlParts& out, std::string& why);

} // namespace tessera::appfw
