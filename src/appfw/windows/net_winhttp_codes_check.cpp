// SPDX-License-Identifier: MIT
//
// A compile-time cross-check of the WinHTTP error codes the mapping names (issue #835).
//
// `net_winhttp_map.hpp` spells the codes as plain integers so a Linux lane can test the table. This
// file is what keeps that honest: it is compiled only on Windows, and a `static_assert` here fails
// the build if a constant and the real `winhttp.h` ever disagree. Without it the transcription
// would be a silent risk; with it, a Windows build is the proof.
//
// It is a translation unit with no symbols - the asserts are the whole content - so it costs a
// compile and nothing else.

#include "tessera/appfw/net_winhttp_map.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace tessera::appfw {

static_assert(kWinHttpNameNotResolved == ERROR_WINHTTP_NAME_NOT_RESOLVED,
              "the mapping's name-not-resolved code drifted from winhttp.h");
static_assert(kWinHttpCannotConnect == ERROR_WINHTTP_CANNOT_CONNECT,
              "the mapping's cannot-connect code drifted from winhttp.h");
static_assert(kWinHttpTimeout == ERROR_WINHTTP_TIMEOUT,
              "the mapping's timeout code drifted from winhttp.h");
static_assert(kWinHttpSecureFailure == ERROR_WINHTTP_SECURE_FAILURE,
              "the mapping's secure-failure code drifted from winhttp.h");
static_assert(kWinHttpInvalidUrl == ERROR_WINHTTP_INVALID_URL,
              "the mapping's invalid-URL code drifted from winhttp.h");
static_assert(kWinHttpOperationCancelled == ERROR_WINHTTP_OPERATION_CANCELLED,
              "the mapping's cancelled code drifted from winhttp.h");
static_assert(kWinHttpConnectionError == ERROR_WINHTTP_CONNECTION_ERROR,
              "the mapping's connection-error code drifted from winhttp.h");
static_assert(kWinHttpUnrecognizedScheme == ERROR_WINHTTP_UNRECOGNIZED_SCHEME,
              "the mapping's unrecognized-scheme code drifted from winhttp.h");

} // namespace tessera::appfw

#endif // _WIN32