// SPDX-License-Identifier: MIT
//
// Pure decode + fallback rules for the freedesktop settings portal (issue #187). See
// include/tessera/hal/portal_appearance.hpp for the contract; nothing here touches a bus, a socket,
// a clock or the filesystem, so every rule is deterministic and unit-tested without a desktop.
//
// Byte order and alignment are the `tessera::dbus::MessageReader`'s job: it is constructed with the
// message's own order flag, so a big-endian peer decodes correctly on any host. This file only
// decides what a decoded value MEANS.

#include "tessera/hal/portal_appearance.hpp"

namespace tessera::hal {

std::string_view to_string(PortalValueError error) noexcept {
    switch (error) {
    case PortalValueError::None:
        return "none";
    case PortalValueError::WrongType:
        return "wrong type";
    case PortalValueError::BadValue:
        return "bad value";
    case PortalValueError::BadSignature:
        return "bad signature";
    }
    return "unknown";
}

std::expected<ColorSchemePreference, PortalValueError>
color_scheme_from_wire_value(std::uint32_t value) noexcept {
    switch (value) {
    case kPortalColorSchemeNoPreference:
        return ColorSchemePreference::NoPreference;
    case kPortalColorSchemePreferDark:
        return ColorSchemePreference::Dark;
    case kPortalColorSchemePreferLight:
        return ColorSchemePreference::Light;
    default:
        // A value the specification does not define: not a preference we can act on.
        return std::unexpected(PortalValueError::BadValue);
    }
}

bool reduced_motion_from_enable_animations(bool animations_enabled) noexcept {
    return !animations_enabled;
}

AppearancePreference with_color_scheme(AppearancePreference appearance,
                                       ColorSchemePreference color_scheme) noexcept {
    appearance.color_scheme = color_scheme;
    if (color_scheme_preference_is_known(color_scheme)) {
        // It is now an answer from the source, not the fallback. An out-of-range value is not an
        // answer, so the assumption bit stays set and the fallback stays in force.
        appearance.assumed.color_scheme = 0;
    }
    return appearance;
}

AppearancePreference with_reduced_motion(AppearancePreference appearance,
                                         bool reduced_motion) noexcept {
    appearance.reduced_motion = reduced_motion;
    appearance.assumed.reduced_motion = 0;
    return appearance;
}

// --- Message decoding (the pure protocol translation) --------------------------------------

namespace {

// Reads one variant (`v`) and dispatches its payload to the mapping for `key`. The reader applies
// the body's byte order and alignment, so a big-endian peer decodes correctly on any host.
std::expected<PortalSettingUpdate, PortalValueError>
read_portal_variant(std::string_view key, tessera::dbus::MessageReader& reader) {
    const auto signature = reader.read_variant_signature();
    if (!signature) {
        return std::unexpected(PortalValueError::BadSignature);
    }
    PortalSettingUpdate update;
    if (key == kPortalColorSchemeKey) {
        // The specified signature is `u`: anything else is a portal speaking a different contract,
        // and the caller keeps its previous value rather than guessing light.
        if (*signature != "u") {
            return std::unexpected(PortalValueError::WrongType);
        }
        const auto scheme = color_scheme_from_wire_value(reader.read_uint32());
        if (!scheme) {
            return std::unexpected(scheme.error());
        }
        update.recognized = true;
        update.is_color_scheme = true;
        update.color_scheme = *scheme;
        return update;
    }
    if (key == kPortalEnableAnimationsKey) {
        if (*signature != "b") {
            return std::unexpected(PortalValueError::WrongType);
        }
        update.recognized = true;
        update.reduced_motion = reduced_motion_from_enable_animations(reader.read_bool());
        return update;
    }
    // A key the framework does not mirror: skip the value and report it unrecognized. The watcher
    // never asks for one, but a signal body is decoded defensively.
    const auto skipped = reader.skip_value(*signature);
    if (!skipped) {
        return std::unexpected(PortalValueError::BadSignature);
    }
    return update;
}

} // namespace

std::expected<PortalSettingUpdate, PortalValueError>
decode_portal_read_reply(std::string_view key, std::span<const std::uint8_t> body,
                         bool big_endian) noexcept {
    tessera::dbus::MessageReader reader(body, big_endian ? tessera::dbus::ByteOrder::Big
                                                         : tessera::dbus::ByteOrder::Little);
    auto update = read_portal_variant(key, reader);
    if (update && reader.failed()) {
        return std::unexpected(PortalValueError::BadValue);
    }
    return update;
}

std::expected<PortalSettingUpdate, PortalValueError>
decode_portal_setting_changed(std::span<const std::uint8_t> body, bool big_endian) noexcept {
    tessera::dbus::MessageReader reader(body, big_endian ? tessera::dbus::ByteOrder::Big
                                                         : tessera::dbus::ByteOrder::Little);
    const auto entered = reader.begin_struct();
    if (!entered) {
        return std::unexpected(PortalValueError::BadSignature);
    }
    const std::string_view name_space = reader.read_string();
    const std::string_view key = reader.read_string();
    if (reader.failed()) {
        return std::unexpected(PortalValueError::BadValue);
    }
    if (name_space != kPortalAppearanceNamespace) {
        // Another namespace's setting: not an appearance change, and not an error.
        return PortalSettingUpdate{};
    }
    auto update = read_portal_variant(key, reader);
    if (update && reader.failed()) {
        return std::unexpected(PortalValueError::BadValue);
    }
    return update;
}

// --- GTK settings.ini fallback --------------------------------------------------------------

namespace {

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// GLib key files write a boolean as `true`/`false`; GSettings' list form writes `true;`/`false;`
// (a one-element ';'-separated list). Both spellings are accepted, nothing else.
bool parse_key_file_bool(std::string_view value, bool& out) {
    while (!value.empty() && (value.back() == ';' || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value == "true" || value == "1") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

GtkSettingsAnswer parse_gtk_settings(std::string_view text) noexcept {
    GtkSettingsAnswer answer;
    answer.appearance = fallback_appearance();
    bool in_settings_section = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view raw_line = text.substr(start, end - start);
        const std::string_view line = trim(raw_line);
        if (line.empty() || line.front() == '#') {
            // Blank and comment lines are legal anywhere and say nothing.
        } else if (line.front() == '[') {
            const std::size_t close = line.find(']');
            // A malformed section header is skipped like any other bad line; GTK writes
            // `[Settings]`.
            in_settings_section =
                close != std::string_view::npos && trim(line.substr(1, close - 1)) == "Settings";
        } else if (in_settings_section) {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos) {
                const std::string_view key = trim(line.substr(0, equals));
                const std::string_view value = trim(line.substr(equals + 1));
                bool parsed = false;
                if (key == kGtkEnableAnimationsKey && parse_key_file_bool(value, parsed)) {
                    // `gtk-enable-animations=false` is the GTK spelling of "reduce motion".
                    answer.appearance = with_reduced_motion(answer.appearance, !parsed);
                    answer.saw_animations = true;
                } else if (key == kGtkApplicationPreferDarkThemeKey &&
                           parse_key_file_bool(value, parsed)) {
                    answer.appearance =
                        with_color_scheme(answer.appearance, parsed ? ColorSchemePreference::Dark
                                                                    : ColorSchemePreference::Light);
                    answer.saw_dark_theme = true;
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return answer;
}

AppearancePreference fallback_appearance() noexcept {
    // Nothing answered: light, motion enabled, and both recorded as assumptions so a consumer
    // cannot mistake this for "the user prefers a light theme with animations".
    return assumed_appearance();
}

} // namespace tessera::hal
