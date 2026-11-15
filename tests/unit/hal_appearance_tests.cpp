// SPDX-License-Identifier: MIT
//
// The OS appearance model and the freedesktop portal rules (issue #187). Pure CPU and display-free:
// no bus, no socket, no clock, no desktop. This is the half that proves the Linux feature's
// decisions on a machine with no session bus (the Linux CI box has none), while
// `appearance_portal` drives the same decoders through a deterministic loopback D-Bus peer.
//
// What each part proves:
//   * the normalized snapshot and its documented fallbacks, including the assumption bits
//     (ADR-023 rule 7) and the packed round trip the platform handoff uses;
//   * the portal value mapping, including the enable-animations INVERSION the feature turns on;
//   * the two message decoders (`Read` reply and `SettingChanged`), built with the real
//     `MessageWriter`, so a big-endian peer and an unknown namespace/key are both covered;
//   * the GTK settings.ini fallback and the absent-portal default.

#include "tessera/dbus/wire.hpp"
#include "tessera/hal/appearance.hpp"
#include "tessera/hal/portal_appearance.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tessera::hal;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "hal_appearance: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Builds a body the way the portal sends it: one variant whose value signature is `signature`.
template<typename Fn>
std::vector<std::uint8_t> body_with_variant(std::string_view signature, Fn&& value) {
    tessera::dbus::MessageWriter writer;
    writer.write_variant(signature, value);
    return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
}

void check_normalized_model() {
    // A default snapshot measured nothing: both assumptions are set and the values are the
    // documented fallbacks (light, motion enabled).
    const AppearancePreference fallback = assumed_appearance();
    expect(!fallback.reduced_motion, "the fallback keeps animations on");
    expect(fallback.color_scheme == ColorSchemePreference::Unspecified,
           "the fallback color scheme is unspecified, not light");
    expect(fallback.assumed.color_scheme == 1 && fallback.assumed.reduced_motion == 1,
           "the fallback records both assumptions");

    // The resolved color scheme distinguishes an answer from the default.
    expect(resolve_color_scheme(ColorSchemePreference::Dark).dark &&
               !resolve_color_scheme(ColorSchemePreference::Dark).assumed,
           "dark is a measured answer");
    expect(!resolve_color_scheme(ColorSchemePreference::Light).dark &&
               !resolve_color_scheme(ColorSchemePreference::Light).assumed,
           "light is a measured answer");
    expect(!resolve_color_scheme(ColorSchemePreference::NoPreference).dark &&
               resolve_color_scheme(ColorSchemePreference::NoPreference).assumed,
           "no-preference resolves to the light default and records the assumption");
    expect(!resolve_color_scheme(ColorSchemePreference::Unspecified).dark &&
               resolve_color_scheme(ColorSchemePreference::Unspecified).assumed,
           "unspecified resolves to the light default and records the assumption");
    expect(!resolve_color_scheme(static_cast<ColorSchemePreference>(9)).dark &&
               resolve_color_scheme(static_cast<ColorSchemePreference>(9)).assumed,
           "an out-of-range preference resolves to the fallback, not a preference");

    // Every enumerator has a name and is known; an out-of-range value is not.
    expect(color_scheme_preference_name(ColorSchemePreference::Dark) == "dark", "dark is named");
    expect(color_scheme_preference_name(static_cast<ColorSchemePreference>(9)) == "unknown",
           "an out-of-range preference reports unknown");
    expect(color_scheme_preference_is_known(ColorSchemePreference::NoPreference),
           "no-preference is a known value");
    expect(!color_scheme_preference_is_known(static_cast<ColorSchemePreference>(9)),
           "an out-of-range value is not known");

    // `appearance_changed` counts an assumption moving even when the values did not: a measurement
    // replacing a guess is a change of what the framework may claim, and the consumer re-evaluates.
    AppearancePreference measured;
    measured.color_scheme = ColorSchemePreference::Light;
    measured.reduced_motion = false;
    measured.assumed.color_scheme = 0;
    measured.assumed.reduced_motion = 0;
    expect(appearance_changed(fallback, measured),
           "a measurement replacing the fallback is a change");
    expect(!appearance_changed(measured, measured), "an identical snapshot is not a change");
}

void check_packed_handoff() {
    // The platform handoff is a single atomic word, so the pack/unpack pair must be exact and
    // total.
    AppearancePreference source;
    source.color_scheme = ColorSchemePreference::Dark;
    source.reduced_motion = true;
    source.assumed.color_scheme = 0;
    source.assumed.reduced_motion = 0;
    const AppearancePreference round_trip = appearance_from_bits(appearance_to_bits(source));
    expect(round_trip == source, "the packed form round-trips a measured dark/reduced snapshot");

    const AppearancePreference fallback_round_trip =
        appearance_from_bits(appearance_to_bits(assumed_appearance()));
    expect(fallback_round_trip == assumed_appearance(),
           "the packed form round-trips the fallback with its assumptions");

    // The packed word carries the color scheme in two bits, which is exactly enough for the four
    // enumerators. A word from another producer therefore cannot name a value this build does not
    // define; the decoder still checks, and `appearance_from_bits` keeps the assumption bit set if
    // the enum ever grows past the mask.
    expect(static_cast<std::uint32_t>(ColorSchemePreference::NoPreference) <= 0x3U,
           "every color-scheme enumerator fits the packed two-bit field");
    AppearancePreference assumed;
    assumed.assumed.color_scheme = 1;
    const AppearancePreference decoded_assumed = appearance_from_bits(appearance_to_bits(assumed));
    expect(decoded_assumed.color_scheme == ColorSchemePreference::Unspecified &&
               decoded_assumed.assumed.color_scheme == 1,
           "an assumed color scheme survives the packed handoff");
}

void check_portal_values() {
    // The color-scheme wire values, including the ones the specification does not define.
    const auto dark = color_scheme_from_wire_value(kPortalColorSchemePreferDark);
    expect(dark && *dark == ColorSchemePreference::Dark, "wire 1 is dark");
    const auto light = color_scheme_from_wire_value(kPortalColorSchemePreferLight);
    expect(light && *light == ColorSchemePreference::Light, "wire 2 is light");
    const auto none = color_scheme_from_wire_value(kPortalColorSchemeNoPreference);
    expect(none && *none == ColorSchemePreference::NoPreference, "wire 0 is no preference");
    expect(!color_scheme_from_wire_value(3), "wire 3 is rejected, not guessed");
    expect(!color_scheme_from_wire_value(0xFFFFFFFFU), "a large wire value is rejected");

    // The inversion: the portal reports whether animations are ALLOWED, the HAL reports reduced
    // motion. Getting this backwards would silently disable every animation on a normal desktop.
    expect(!reduced_motion_from_enable_animations(true),
           "animations enabled means motion is NOT reduced");
    expect(reduced_motion_from_enable_animations(false),
           "animations disabled means motion IS reduced");

    // Merging records the key as measured.
    AppearancePreference appearance = assumed_appearance();
    appearance = with_color_scheme(appearance, ColorSchemePreference::Dark);
    expect(appearance.color_scheme == ColorSchemePreference::Dark &&
               appearance.assumed.color_scheme == 0,
           "a decoded color scheme is recorded as measured");
    appearance = with_reduced_motion(appearance, true);
    expect(appearance.reduced_motion && appearance.assumed.reduced_motion == 0,
           "a decoded motion preference is recorded as measured");
}

void check_read_reply_decode() {
    // color-scheme: one variant `u` holding 1 (dark).
    {
        const auto body = body_with_variant(
            "u", [](tessera::dbus::MessageWriter& writer) { writer.write_uint32(1); });
        const auto update = decode_portal_read_reply(kPortalColorSchemeKey, body, false);
        expect(update && update->recognized && update->is_color_scheme,
               "a color-scheme Read reply is recognized");
        expect(update->color_scheme == ColorSchemePreference::Dark, "the reply decodes dark");
    }
    // enable-animations: one variant `b` holding false, which is reduced motion.
    {
        const auto body = body_with_variant(
            "b", [](tessera::dbus::MessageWriter& writer) { writer.write_bool(false); });
        const auto update = decode_portal_read_reply(kPortalEnableAnimationsKey, body, false);
        expect(update && update->recognized && !update->is_color_scheme,
               "an enable-animations Read reply is recognized");
        expect(update->reduced_motion, "animations disabled decodes to reduced motion");
    }
    // A wrong signature for the key is an error, not a guess.
    {
        const auto body = body_with_variant(
            "s", [](tessera::dbus::MessageWriter& writer) { writer.write_string("dark"); });
        const auto update = decode_portal_read_reply(kPortalColorSchemeKey, body, false);
        expect(!update && update.error() == PortalValueError::WrongType,
               "a string answer to color-scheme is a wrong-type error");
    }
    // A truncated body is a bad value, never an out-of-bounds read.
    {
        const std::vector<std::uint8_t> body{static_cast<std::uint8_t>('u')};
        const auto update = decode_portal_read_reply(kPortalColorSchemeKey, body, false);
        expect(!update, "a truncated variant is rejected");
    }
    // The same reply in big-endian order decodes identically: the reader applies the message order.
    // A variant is the signature type (one length byte, the type code, a NUL), padded to the
    // value's alignment, then the value itself — here `u` value 2 (prefer light) written
    // big-endian.
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x01); // signature length
        body.push_back('u');
        body.push_back(0x00);
        body.push_back(0x00); // padding to the 4-byte scalar alignment
        body.push_back(0x00);
        body.push_back(0x00);
        body.push_back(0x00);
        body.push_back(0x02); // 2 = prefer light, big-endian
        const auto update = decode_portal_read_reply(kPortalColorSchemeKey, body, true);
        expect(update && update->color_scheme == ColorSchemePreference::Light,
               "a big-endian reply decodes with the message's byte order");
    }
}

void check_setting_changed_decode() {
    // The signal body is `(ssv)`: namespace, key, variant.
    const auto make_body = [](std::string_view name_space, std::string_view key,
                              std::string_view signature, bool animations) {
        tessera::dbus::MessageWriter writer;
        writer.begin_struct();
        writer.write_string(name_space);
        writer.write_string(key);
        writer.write_variant(signature, [animations](tessera::dbus::MessageWriter& inner) {
            inner.write_bool(animations);
        });
        writer.end_struct();
        return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
    };
    {
        const auto body =
            make_body(kPortalAppearanceNamespace, kPortalEnableAnimationsKey, "b", false);
        const auto update = decode_portal_setting_changed(body, false);
        expect(update && update->recognized && update->reduced_motion,
               "a SettingChanged for enable-animations decodes reduced motion");
    }
    // Another namespace is not an error and not an appearance change: the watcher ignores it.
    {
        const auto body = make_body("org.example.other", kPortalEnableAnimationsKey, "b", true);
        const auto update = decode_portal_setting_changed(body, false);
        expect(update && !update->recognized,
               "a signal for another namespace is ignored, not failed");
    }
    // A malformed body fails as a value.
    {
        const std::vector<std::uint8_t> body{0x00};
        expect(!decode_portal_setting_changed(body, false), "a truncated signal body is rejected");
    }
}

void check_gtk_fallback() {
    // No file content at all: the documented default, nothing assumed measured.
    {
        const GtkSettingsAnswer answer = parse_gtk_settings("");
        expect(!answer.saw_animations && !answer.saw_dark_theme, "an empty file says nothing");
        expect(!answer.appearance.reduced_motion && answer.appearance.assumed.reduced_motion == 1,
               "an empty file keeps animations on as an assumption");
    }
    // Both keys, GTK key-file boolean spelling.
    {
        const GtkSettingsAnswer answer = parse_gtk_settings(R"([Settings]
gtk-enable-animations=false
gtk-application-prefer-dark-theme=true
)");
        expect(answer.saw_animations && answer.saw_dark_theme, "both keys are read");
        expect(answer.appearance.reduced_motion, "gtk-enable-animations=false is reduced motion");
        expect(answer.appearance.color_scheme == ColorSchemePreference::Dark,
               "prefer-dark-theme=true is a dark color scheme");
        expect(answer.appearance.assumed.reduced_motion == 0 &&
                   answer.appearance.assumed.color_scheme == 0,
               "what the file said is recorded as measured");
    }
    // Only one key present: the other keeps the documented fallback with its assumption.
    {
        const GtkSettingsAnswer answer =
            parse_gtk_settings("[Settings]\ngtk-enable-animations=true\n");
        expect(answer.saw_animations && !answer.saw_dark_theme, "only one key was read");
        expect(!answer.appearance.reduced_motion, "animations enabled is not reduced motion");
        expect(answer.appearance.assumed.color_scheme == 1,
               "the key the file omitted stays assumed");
    }
    // Keys outside [Settings] and comments are ignored; a malformed line is skipped, not fatal.
    {
        const GtkSettingsAnswer answer = parse_gtk_settings(R"(# a comment
[Other]
gtk-enable-animations=false
[Settings]
not a key value line
gtk-application-prefer-dark-theme = true ;
)");
        expect(!answer.saw_animations, "a key in another section is ignored");
        expect(answer.saw_dark_theme, "whitespace and the GSettings list form are accepted");
    }
    // The absent-portal answer is the same documented fallback.
    expect(fallback_appearance() == assumed_appearance(),
           "the absent-portal fallback matches the normalized default");
}

} // namespace

int main() {
    check_normalized_model();
    check_packed_handoff();
    check_portal_values();
    check_read_reply_decode();
    check_setting_changed_decode();
    check_gtk_fallback();
    if (failures == 0) {
        std::cout << "hal_appearance: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
