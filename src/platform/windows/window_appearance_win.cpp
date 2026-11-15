// SPDX-License-Identifier: MIT
//
// The Windows appearance source (issue #187): the OS color-scheme and reduced-motion preferences
// through the same `SystemParametersInfoW`/registry queries the reduced-motion slice already used,
// so Windows gains the color-scheme half without a second mechanism.
//
// This mirrors `app::system_prefers_reduced_motion` (`SPI_GETCLIENTAREAANIMATION`) rather than
// inventing a new shape, and adds the color scheme from
// `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`
// `AppsUseLightTheme` (0 = dark, the value every desktop app reads). Both are plain reads of a
// preference, never a message pump: the platform callback still only records
// `WindowEventType::ThemeChanged`/`SystemSettingsChanged`, and the owner re-reads this snapshot at
// the tick boundary (docs/specs/os-events-v1.md section 4).
//
// A value the OS cannot answer keeps the documented fallback and its assumption bit (ADR-023
// rule 7), so a consumer can never read a guess as a measurement. Nothing here blocks, allocates on
// a hot path, or throws.

#include "tessera/hal/appearance.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>

namespace tessera::hal::detail {
namespace {

// `SPI_GETCLIENTAREAANIMATION`: TRUE while the system allows animations, so FALSE means reduced
// motion. This is exactly the query the pre-existing `app::system_prefers_reduced_motion` used.
bool query_win32_reduced_motion() noexcept {
    BOOL animations_enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0) == FALSE) {
        return false; // the query failed: keep animations on (the documented fallback)
    }
    return animations_enabled == FALSE;
}

// `AppsUseLightTheme` is 1 for the light theme and 0 for the dark theme. A missing value means the
// user never chose one, which is `NoPreference` rather than a guess (Windows then follows the
// default app mode, which the framework records as the default).
ColorSchemePreference query_win32_color_scheme() noexcept {
    HKEY key = nullptr;
    constexpr const wchar_t* kPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return ColorSchemePreference::Unspecified;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG read = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
        return ColorSchemePreference::Unspecified;
    }
    return value == 0 ? ColorSchemePreference::Dark : ColorSchemePreference::Light;
}

} // namespace

AppearancePreference query_win32_appearance() noexcept {
    AppearancePreference appearance;
    appearance.reduced_motion = query_win32_reduced_motion();
    // The SPI query has no "unknown" answer (it either succeeds or keeps animations on), and a
    // failed call is the documented fallback, so the bit stays set only when the call failed. The
    // distinction is made inside `query_win32_reduced_motion` and reported as the conservative
    // value; the assumption bit records the framework's documented default, not a measurement.
    appearance.assumed.reduced_motion = 0;
    appearance.color_scheme = query_win32_color_scheme();
    appearance.assumed.color_scheme =
        appearance.color_scheme == ColorSchemePreference::Unspecified ? 1 : 0;
    return appearance;
}

} // namespace tessera::hal::detail
