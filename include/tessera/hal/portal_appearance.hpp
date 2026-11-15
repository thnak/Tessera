// SPDX-License-Identifier: MIT
//
// Decoding rules for the freedesktop settings portal (docs/specs/os-events-v1.md section 5, issue
// #187). Everything here is PURE: it turns the raw D-Bus values the portal reports into the HAL's
// normalized appearance snapshot, and parses the fallback settings file, without a bus, a socket, a
// clock or a desktop. That is what makes the Linux half testable on a machine with no session bus
// (the Linux CI box and the build server have none by default).
//
// The portal contract this decodes (org.freedesktop.portal.Settings, version 2):
//   * `Read(namespace, key)` returns one `variant`; the appearance namespace is
//     `org.freedesktop.appearance` and the two keys we mirror are `color-scheme` and
//     `enable-animations`.
//   * `color-scheme` is a `u`: 0 = no preference, 1 = prefer dark, 2 = prefer light.
//   * `enable-animations` is a `b`: false means the user asked apps to reduce motion (the inverted
//     form the HAL reports as `reduced_motion`).
//   * `SettingChanged(namespace, key, value)` is the change signal; `value` is the same variant.
//   * `ReadOne`/`ReadAll` are later additions; a portal that only speaks version 1 still answers
//     `Read`, which is what the watcher uses.
//
// A value the portal sends that we do not recognise is `Unspecified`/assumed, never a guess
// (ADR-023 rule 5: ignore the unknown value and report one diagnostic).

#pragma once

#include "tessera/dbus/wire.hpp"
#include "tessera/hal/appearance.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace tessera::hal {

// The portal's `org.freedesktop.appearance` namespace, and the two keys the framework mirrors.
inline constexpr std::string_view kPortalAppearanceNamespace = "org.freedesktop.appearance";
inline constexpr std::string_view kPortalColorSchemeKey = "color-scheme";
inline constexpr std::string_view kPortalEnableAnimationsKey = "enable-animations";

// The `color-scheme` wire values the portal specification defines.
inline constexpr std::uint32_t kPortalColorSchemeNoPreference = 0;
inline constexpr std::uint32_t kPortalColorSchemePreferDark = 1;
inline constexpr std::uint32_t kPortalColorSchemePreferLight = 2;

enum class PortalValueError : std::uint8_t {
    None = 0,
    // The variant signature is not the one the key is specified with (a `u` for color-scheme, a `b`
    // for enable-animations). The caller keeps its previous value and reports one diagnostic.
    WrongType,
    // The value is inside a variant of the right type but its payload is truncated or out of range.
    BadValue,
    // The signature text itself is malformed (the codec rejects it before this point).
    BadSignature,
};

[[nodiscard]] std::string_view to_string(PortalValueError error) noexcept;

// Maps the portal's `color-scheme` wire value to the HAL preference. The specification defines
// 0 = no preference, 1 = prefer dark, 2 = prefer light. Any other value is `BadValue` and the
// caller keeps its current preference; nothing is invented for an unknown value. This is a pure
// function over one integer, so the mapping is tested with three constants and no bus at all.
[[nodiscard]] std::expected<ColorSchemePreference, PortalValueError>
color_scheme_from_wire_value(std::uint32_t value) noexcept;

// Maps the portal's `enable-animations` boolean to the HAL preference. It is the inversion the
// whole feature turns on: the portal reports whether animations are ALLOWED, the framework mirrors
// whether motion should be REDUCED. Pure, so the inversion is pinned by a test rather than implied
// by a call site.
[[nodiscard]] bool reduced_motion_from_enable_animations(bool animations_enabled) noexcept;

// Merges one decoded key into a snapshot, recording that the key is now measured rather than
// assumed. Kept as small functions so the watcher's state machine has no branch of its own for a
// key it already decoded.
[[nodiscard]] AppearancePreference with_color_scheme(AppearancePreference appearance,
                                                     ColorSchemePreference color_scheme) noexcept;
[[nodiscard]] AppearancePreference with_reduced_motion(AppearancePreference appearance,
                                                       bool reduced_motion) noexcept;

// --- Message decoding (the whole protocol translation, pure) --------------------------------
//
// These two functions are the entire portal protocol surface the watcher has to get right: a
// `Read` reply carries one variant, and `SettingChanged` carries (namespace, key, variant). Both
// take the D-Bus MESSAGE BODY (not the framed message) plus the body's byte order, so a test can
// build one with `MessageWriter` and check the decode without a bus, a socket or a thread. That is
// how the Linux half stays deterministic on a box with no session bus.

// What one `Read` reply or one `SettingChanged` payload says. `key` is the appearance key that was
// addressed; `value` is the decoded preference. A key the framework does not mirror leaves
// `recognized` false and the caller ignores it (rule 5: unknown values are ignored).
struct PortalSettingUpdate {
    bool recognized = false;
    // Valid when `recognized`; for `color-scheme` this is `preference`-bearing, for
    // `enable-animations` the `reduced_motion` boolean is the meaning.
    ColorSchemePreference color_scheme = ColorSchemePreference::Unspecified;
    bool reduced_motion = false;
    bool is_color_scheme = false;
};

// Decodes the body of a `org.freedesktop.portal.Settings.Read` reply for `key`: the signature is
// `v` (one variant), whose value signature is `u` for `color-scheme` and `b` for
// `enable-animations`. `key` selects which decode applies; an unknown key is not an error (the
// watcher only asks for the two it mirrors).
[[nodiscard]] std::expected<PortalSettingUpdate, PortalValueError>
decode_portal_read_reply(std::string_view key, std::span<const std::uint8_t> body,
                         bool big_endian) noexcept;

// Decodes the body of a `SettingChanged` signal: `(ssv)` — namespace, key, variant. A signal for a
// namespace/key the framework does not mirror returns `recognized == false` rather than failing, so
// a portal that broadcasts unrelated settings costs one string comparison.
[[nodiscard]] std::expected<PortalSettingUpdate, PortalValueError>
decode_portal_setting_changed(std::span<const std::uint8_t> body, bool big_endian) noexcept;

// --- The no-portal fallback -----------------------------------------------------------------
//
// The portal is the primary source, but a session without one (a bare X session, a container, the
// CI box) still has usable answers: GTK's `settings.ini` records the same two preferences and is
// what the portal itself reads on a GTK desktop. `settings.ini` syntax is a bounded subset of GLib
// key files: `[Settings]` section, `key=value` lines, `#` comments, and GSettings' ';'-separated
// list form for values. Only the two keys this feature needs are read.
//
// The fallback is intentionally a pure parser over TEXT so its behavior is unit-testable: the
// platform half only supplies the file's bytes and a path to `parse_gtk_settings`.

// GTK's two keys, spelled as they appear in `settings.ini`.
inline constexpr std::string_view kGtkEnableAnimationsKey = "gtk-enable-animations";
inline constexpr std::string_view kGtkApplicationPreferDarkThemeKey =
    "gtk-application-prefer-dark-theme";

// The answer of the file-based fallback: the same two preferences plus which of them the file
// actually spoke about. A file that omits a key leaves that key's documented fallback in place with
// its assumption set, so the resolution is never presented as a measurement it is not.
struct GtkSettingsAnswer {
    AppearancePreference appearance;
    bool saw_animations = false;
    bool saw_dark_theme = false;
};

// Parses GTK `settings.ini` text. Unknown sections/keys are ignored (the file holds many unrelated
// settings), a malformed line is skipped rather than rejecting the whole file, and the input is
// bounded by the caller's max size. `gtk-enable-animations=false` reports reduced motion;
// `gtk-application-prefer-dark-theme=true` reports a dark color-scheme. Everything the file does
// not say keeps its documented fallback and stays assumed.
[[nodiscard]] GtkSettingsAnswer parse_gtk_settings(std::string_view text) noexcept;

// The documented answer when neither the portal nor a settings file has anything to say: light,
// motion enabled, both assumed. A test asserts the absent-portal path through this function so the
// "no desktop" behavior is pinned rather than implied.
[[nodiscard]] AppearancePreference fallback_appearance() noexcept;

} // namespace tessera::hal
