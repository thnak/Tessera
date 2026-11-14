// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The theme asset of docs/specs/stylesheet-v1.md section 9.1: the JSON file named by
// `assets.theme`, its versioned binary blob, and the generated `:root` rule the active theme
// injects. Parsing is build time (the CLI); a running app only loads the blob.
namespace tessera::ui::style {

// The built-in theme and the reserved chrome pair (stylesheet-v1.md section 9.2). They are
// resolved from the theme asset before the style engine runs, so a project without a theme asset
// still has a coherent window/loading surface.
inline constexpr std::string_view kDefaultThemeId = "default";
inline constexpr std::string_view kChromeBackgroundToken = "--window-background";
inline constexpr std::string_view kChromeForegroundToken = "--window-foreground";
inline constexpr std::string_view kChromeBackgroundValue = "#ffffff";
inline constexpr std::string_view kChromeForegroundValue = "#111111";

// One custom-property declaration a theme defines, in asset order.
struct ThemeToken {
    std::string name;
    std::string value;

    friend bool operator==(const ThemeToken&, const ThemeToken&) = default;
};

struct Theme {
    std::string id;
    // The theme's own SemVer `MAJOR.MINOR.PATCH` (ADR-047 item 4, stylesheet-v1.md section 9.1),
    // empty when the asset does not version this theme. It is not a token and is never injected.
    std::string version;
    std::vector<ThemeToken> tokens; // asset order

    [[nodiscard]] const ThemeToken* find(std::string_view name) const;

    friend bool operator==(const Theme&, const Theme&) = default;
};

// A parsed `assets.theme` file. `themes` is ordered: the built-in `default` is first, then the
// keys of `themes` in document order (a `themes.default` entry supplies the first theme's tokens
// instead of adding a second one). `warnings` holds the `TESSERA_THEME_TOKEN_MISSING` findings,
// which are derived from `themes` and are recomputed by the blob loader.
struct ThemeAsset {
    std::uint32_t schema_version = 1;
    std::string start = std::string(kDefaultThemeId);
    std::vector<Theme> themes;
    // True when the asset carries a `themes.default` entry. The built-in `default` is always
    // present; only an authored one takes part in the cross-theme token-missing scan.
    bool default_authored = false;
    std::vector<std::string> warnings;

    [[nodiscard]] const Theme* find(std::string_view id) const;

    friend bool operator==(const ThemeAsset&, const ThemeAsset&) = default;
};

// The spec's typed errors (stylesheet-v1.md section 9.1). A shape violation of the schema-1 asset
// (missing or wrong-typed `schemaVersion`/`themes`, a malformed token name or value) is `Version`:
// the file is not a theme asset of the supported schema.
enum class ThemeErrorCode : std::uint8_t {
    Version,      // TESSERA_THEME_VERSION
    Id,           // TESSERA_THEME_ID
    StartUnknown, // TESSERA_THEME_START_UNKNOWN
    VersionPin,   // TESSERA_THEME_VERSION_PIN
};

[[nodiscard]] constexpr const char* theme_error_code_name(ThemeErrorCode code) {
    switch (code) {
    case ThemeErrorCode::Version:
        return "TESSERA_THEME_VERSION";
    case ThemeErrorCode::Id:
        return "TESSERA_THEME_ID";
    case ThemeErrorCode::StartUnknown:
        return "TESSERA_THEME_START_UNKNOWN";
    case ThemeErrorCode::VersionPin:
        return "TESSERA_THEME_VERSION_PIN";
    }
    return "TESSERA_THEME_VERSION";
}

struct ThemeError {
    ThemeErrorCode code = ThemeErrorCode::Version;
    std::string message;
};

// Build-time parse of the asset text with the harness JSON decoder; never throws. Validates the
// version, the theme-id pattern, token names and values, and `start`, and returns the asset with
// its `TESSERA_THEME_TOKEN_MISSING` warning list.
[[nodiscard]] std::expected<ThemeAsset, ThemeError> parse_theme_asset(std::string_view json);

// True when `version` is `MAJOR.MINOR.PATCH` (decimal, no leading zeros, no pre-release or build
// suffix), the only form a theme version takes.
[[nodiscard]] bool is_theme_version(std::string_view version);

// True when `pin` is `MAJOR`, `MAJOR.MINOR` or `MAJOR.MINOR.PATCH` in the same form.
[[nodiscard]] bool is_theme_version_pin(std::string_view pin);

// A theme satisfies a pin when the pin is a component-wise prefix of its version: pin `1` accepts
// every `1.x.y`, `1.2` accepts every `1.2.y`, `1.2.3` accepts only `1.2.3`. An unversioned theme
// (empty `version`) never satisfies a pin.
[[nodiscard]] bool theme_version_satisfies_pin(std::string_view version, std::string_view pin);

// One `assets.themePins` entry (project-config-v1.md): the theme id and the pin it must satisfy.
struct ThemePin {
    std::string theme;
    std::string pin;
};

// Checks every pin against the asset. A pin naming an unknown theme, a theme without a `version` or
// a version the pin does not accept is `TESSERA_THEME_VERSION_PIN`, naming the theme and both
// versions. Returns the first violation in `pins` order.
[[nodiscard]] std::optional<ThemeError> check_theme_pins(const ThemeAsset& asset,
                                                         std::span<const ThemePin> pins);

// The built-in asset: one `default` theme defining the reserved chrome pair.
[[nodiscard]] ThemeAsset default_theme_asset();

// The prepared sheet of one theme: a single `:root` rule holding only that theme's tokens, plus the
// built-in value of a reserved token the theme does not define (stylesheet-v1.md section 9.2).
// An unknown id yields an empty sheet. The result is appended to a cascade with `Origin::Theme`.
[[nodiscard]] PreparedStyleSheet prepare_theme_stylesheet(const ThemeAsset& asset,
                                                          std::string_view id);

// Serialized theme asset (docs/specs/compiled-stylesheet-v1.md "Theme asset blob"). The bytes are
// deterministic: identical assets always serialize to identical bytes, and the FNV-1a 64 payload
// hash in the header is verified on load.
inline constexpr std::uint32_t kThemeBlobFormatVersion = 2;

enum class ThemeBlobErrorCode : std::uint8_t {
    Truncated,
    BadMagic,
    UnsupportedFormatVersion,
    HashMismatch,
    Corrupt,
};

[[nodiscard]] constexpr const char* theme_blob_error_code_name(ThemeBlobErrorCode code) {
    switch (code) {
    case ThemeBlobErrorCode::Truncated:
        return "Truncated";
    case ThemeBlobErrorCode::BadMagic:
        return "BadMagic";
    case ThemeBlobErrorCode::UnsupportedFormatVersion:
        return "UnsupportedFormatVersion";
    case ThemeBlobErrorCode::HashMismatch:
        return "HashMismatch";
    case ThemeBlobErrorCode::Corrupt:
        return "Corrupt";
    }
    return "Corrupt";
}

struct ThemeBlobError {
    ThemeBlobErrorCode code = ThemeBlobErrorCode::Corrupt;
    std::string message;
};

// One embedded theme as codegen emits it (docs/specs/compiled-stylesheet-v1.md "Theme asset blob"):
// `<module>_theme.hpp` defines a `kThemeBlob` array and a `kEmbeddedTheme` record of this type.
struct EmbeddedTheme {
    std::span<const std::uint8_t> bytes;
};

[[nodiscard]] std::vector<std::uint8_t> serialize_theme_asset(const ThemeAsset& asset);

// Validates the header, the payload hash/size and every count and string length before returning;
// never reads outside `bytes` and never throws on malformed input.
[[nodiscard]] std::expected<ThemeAsset, ThemeBlobError>
load_theme_asset_blob(std::span<const std::uint8_t> bytes);

} // namespace tessera::ui::style
