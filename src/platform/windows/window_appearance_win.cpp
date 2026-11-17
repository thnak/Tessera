// SPDX-License-Identifier: MIT
//
// The Windows user-setting source (issue #187 and the S7 ClearType fact): the OS color scheme,
// reduced motion and text-smoothing preferences through the same `SystemParametersInfoW`/registry
// queries, so Windows gains each new user preference without a second mechanism.
//
// The ClearType geometry query lives here rather than in `window_display_win.cpp` for the same
// reason as the appearance snapshot: it reads a USER setting, and it keeps the per-monitor display
// TU inside its file-size budget. `query_win32_pixel_geometry` is declared in
// `window_display_win.hpp` next to the other display capabilities it feeds.
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
#include "tessera/hal/display.hpp"
// The platform header owns the SDK include order (windows.h before dbt.h) and the _WIN32_WINNT /
// WINVER definitions, so this TU does not repeat them.
#include "window_display_win.hpp"

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

PixelGeometry query_win32_pixel_geometry() noexcept {
    // Smoothing off is answered by the first query alone: the OS will not rasterize subpixel
    // coverage, so the type and orientation (which may still hold a stale ClearType value) say
    // nothing about what the screen will show.
    BOOL smoothing = FALSE;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0) == FALSE) {
        return PixelGeometry::Unknown;
    }
    if (smoothing == FALSE) {
        return clear_type_geometry(false, FontSmoothingOrientation::Rgb, FontSmoothingType::None);
    }

    UINT type = 0;
    UINT orientation = 0;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0) == FALSE ||
        SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &orientation, 0) == FALSE) {
        return PixelGeometry::Unknown;
    }
    // Map the raw SDK values and let the pure mapper decide: a value this build does not understand
    // is reported as Unknown rather than passed off as a stripe (ADR-023 rule 7).
    FontSmoothingType smoothing_type = FontSmoothingType::None;
    if (type == FE_FONTSMOOTHINGSTANDARD) {
        smoothing_type = FontSmoothingType::Standard;
    } else if (type == FE_FONTSMOOTHINGCLEARTYPE) {
        smoothing_type = FontSmoothingType::ClearType;
    } else {
        return PixelGeometry::Unknown;
    }
    FontSmoothingOrientation stripe = FontSmoothingOrientation::Rgb;
    if (orientation == FE_FONTSMOOTHINGORIENTATIONBGR) {
        stripe = FontSmoothingOrientation::Bgr;
    } else if (orientation != FE_FONTSMOOTHINGORIENTATIONRGB) {
        return PixelGeometry::Unknown;
    }
    return clear_type_geometry(true, stripe, smoothing_type);
}

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
