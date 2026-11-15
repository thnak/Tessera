// SPDX-License-Identifier: MIT
//
// The app appearance preferences the OS reports: color scheme and reduced motion
// (docs/specs/os-events-v1.md sections 2 and 5, ADR-023 rule 7).
//
// Windows has answered the reduced-motion half since the first window driver
// (`app::system_prefers_reduced_motion`, `SPI_GETCLIENTAREAANIMATION`); the color-scheme half
// (`UISettings::GetColorValue(UIColorType::Foreground)`) is the same kind of query. Linux answers
// both through the freedesktop settings portal (`org.freedesktop.appearance` `color-scheme` and
// `enable-animations`), with a file-based fallback when no portal is running.
//
// This header is the platform-neutral half so no consumer sees an OS handle or an OS message: the
// HAL reports a normalized snapshot, and a change arrives as
// `WindowEventType::SystemSettingsChanged` (the existing settings event, which carries no value;
// the owner re-reads the snapshot at the tick boundary, exactly as the Windows reduced-motion path
// already does).
//
// ADR-023 rule 7 ("record the assumption"): a platform that cannot answer must not report a made-up
// preference as if it were measured. `ColorScheme::Unspecified` and the matching `assumed` bits are
// that record, and `resolve_*` apply the documented fallbacks with the assumption flagged.

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::hal {

// The OS color-scheme preference, normalized. `Unspecified` is the documented "the platform did not
// answer" value, NOT `Light`: a consumer that must pick something resolves it through
// `resolve_color_scheme`, which reports the choice as an assumption, so a dark-only app can tell a
// real light preference from a platform that said nothing.
enum class ColorSchemePreference : std::uint8_t {
    Unspecified = 0,
    Light,
    Dark,
    // The OS/user asked for no preference at all (the freedesktop portal's value 0). It is distinct
    // from `Unspecified`: this is an answer, and the documented fallback for it is `Light`.
    NoPreference,
};

// The spelling for diagnostics and the harness metrics dump.
[[nodiscard]] constexpr std::string_view
color_scheme_preference_name(ColorSchemePreference preference) noexcept {
    switch (preference) {
    case ColorSchemePreference::Unspecified:
        return "unspecified";
    case ColorSchemePreference::Light:
        return "light";
    case ColorSchemePreference::Dark:
        return "dark";
    case ColorSchemePreference::NoPreference:
        return "no-preference";
    }
    // An out-of-range value is not a preference; consumers reject it with
    // color_scheme_preference_is_known() before reporting anything.
    return "unknown";
}

// True for the values the enum defines; false for an out-of-range wire value, which every consumer
// treats as `Unspecified` (ADR-023 rule 5: ignore the unknown value, report one diagnostic, never
// crash).
[[nodiscard]] constexpr bool
color_scheme_preference_is_known(ColorSchemePreference preference) noexcept {
    switch (preference) {
    case ColorSchemePreference::Unspecified:
    case ColorSchemePreference::Light:
    case ColorSchemePreference::Dark:
    case ColorSchemePreference::NoPreference:
        return true;
    }
    return false;
}

// The resolved color scheme and whether the resolution is the documented fallback rather than an
// answer (ADR-023 rule 7). `dark` is false whenever `assumed` is true.
struct ResolvedColorScheme {
    bool dark = false;
    bool assumed = true;
};

// Resolves a reported preference to light/dark. `NoPreference` resolves to light and stays an
// assumption (the user expressed no preference, so the framework's default applies and is recorded
// as the default, not as a measurement). `Light`/`Dark` are answers; anything out of range is
// `Unspecified` and resolves to light as an assumption.
[[nodiscard]] constexpr ResolvedColorScheme
resolve_color_scheme(ColorSchemePreference preference) noexcept {
    switch (preference) {
    case ColorSchemePreference::Dark:
        return ResolvedColorScheme{true, false};
    case ColorSchemePreference::Light:
        return ResolvedColorScheme{false, false};
    case ColorSchemePreference::NoPreference:
        // An answer of "no preference": the framework's light default applies, and it is recorded
        // as the default rather than as a light preference the user never expressed.
        return ResolvedColorScheme{false, true};
    case ColorSchemePreference::Unspecified:
        break;
    }
    return ResolvedColorScheme{false, true};
}

// One OS appearance snapshot: the two preferences the framework mirrors, each with an "assumed" bit
// so a consumer can never read a fallback as a measurement. It is a plain value: no heap, no OS
// handle, cheap to copy, and the same shape on every platform (a platform that has no source leaves
// the defaults, which are the documented fallbacks with every assumption set).
struct AppearancePreference {
    ColorSchemePreference color_scheme = ColorSchemePreference::Unspecified;
    // True when the OS asks apps to reduce motion. Unlike the color scheme there is no
    // "unspecified" enumerator: animations-on is the documented fallback everywhere, so a platform
    // that cannot answer reports `false` and sets `reduced_motion_assumed`.
    // `AppearancePreference::assumed` is the one place that decision is recorded.
    bool reduced_motion = false;
    struct Assumptions {
        std::uint8_t color_scheme : 1;   // the color scheme is the fallback, not measured
        std::uint8_t reduced_motion : 1; // the motion preference is the fallback, not measured

        [[nodiscard]] constexpr bool operator==(const Assumptions&) const = default;
    };
    // Both bits set: a default-constructed snapshot measured nothing.
    Assumptions assumed{1, 1};

    [[nodiscard]] constexpr bool operator==(const AppearancePreference&) const = default;
};

// The documented fallback: light/animations-on, both recorded as assumptions. Every platform
// without a settings source returns it, so a consumer's code path is the same everywhere.
[[nodiscard]] constexpr AppearancePreference assumed_appearance() noexcept {
    return AppearancePreference{};
}

// True when two snapshots differ in a way a consumer must react to. The assumption bits count: an
// unmeasured fallback being replaced by a measurement of the same value is still a change of what
// the framework is allowed to claim, and the reduced-motion consumer must re-evaluate either way.
[[nodiscard]] constexpr bool appearance_changed(const AppearancePreference& previous,
                                                const AppearancePreference& current) noexcept {
    return !(previous == current);
}

// Packed form of a snapshot, for the lock-free handoff between a platform reader thread and the UI
// thread (the frame path never takes a lock). Both directions are total: the packed value always
// decodes to a valid snapshot, and an out-of-range enum value in the source decodes to
// `Unspecified`/assumed rather than becoming a preference.
namespace detail {
inline constexpr std::uint32_t kColorSchemeShift = 0;
inline constexpr std::uint32_t kColorSchemeMask = 0x3U;
inline constexpr std::uint32_t kReducedMotionBit = 1U << 2;
inline constexpr std::uint32_t kAssumedColorSchemeBit = 1U << 3;
inline constexpr std::uint32_t kAssumedReducedMotionBit = 1U << 4;
} // namespace detail

[[nodiscard]] constexpr std::uint32_t
appearance_to_bits(const AppearancePreference& appearance) noexcept {
    const auto scheme =
        static_cast<std::uint32_t>(appearance.color_scheme) & detail::kColorSchemeMask;
    std::uint32_t bits = scheme << detail::kColorSchemeShift;
    if (appearance.reduced_motion) {
        bits |= detail::kReducedMotionBit;
    }
    if (appearance.assumed.color_scheme != 0) {
        bits |= detail::kAssumedColorSchemeBit;
    }
    if (appearance.assumed.reduced_motion != 0) {
        bits |= detail::kAssumedReducedMotionBit;
    }
    return bits;
}

[[nodiscard]] constexpr AppearancePreference appearance_from_bits(std::uint32_t bits) noexcept {
    AppearancePreference appearance;
    const auto scheme = static_cast<ColorSchemePreference>((bits >> detail::kColorSchemeShift) &
                                                           detail::kColorSchemeMask);
    // A packed value from a newer producer could name an enumerator this build does not define;
    // that is `Unspecified` with the assumption set, never a preference (ADR-023 rule 5).
    appearance.color_scheme =
        color_scheme_preference_is_known(scheme) ? scheme : ColorSchemePreference::Unspecified;
    appearance.reduced_motion = (bits & detail::kReducedMotionBit) != 0;
    appearance.assumed.color_scheme = (bits & detail::kAssumedColorSchemeBit) != 0 ? 1 : 0;
    appearance.assumed.reduced_motion = (bits & detail::kAssumedReducedMotionBit) != 0 ? 1 : 0;
    if (!color_scheme_preference_is_known(appearance.color_scheme)) {
        appearance.assumed.color_scheme = 1;
    }
    return appearance;
}

} // namespace tessera::hal
