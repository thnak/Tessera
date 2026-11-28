// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/theme_asset.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The opt-in bundled font pack (ADR-047 item 4, issue #280, docs/specs/theme-versions-v1.md section
// 5): a manifest that lists the pack's font files with their SHA-256, and the theme-side pin that
// names the pack version a theme was designed against. Parsing is build time; a running app never
// reads either.
namespace tessera::ui::style {

struct FontPackFile {
    std::string file; // relative to the manifest
    std::string family;
    std::uint16_t weight = 400;
    std::string style;  // "normal" or "italic"
    std::string sha256; // 64 lowercase hex characters
    std::uint64_t bytes = 0;
};

struct FontPack {
    std::string id;
    std::string version; // MAJOR.MINOR.PATCH, the pack's own version
    std::vector<FontPackFile> fonts;
};

enum class FontPackErrorCode : std::uint8_t {
    Manifest, // TESSERA_FONT_PACK_MANIFEST
    Pin,      // TESSERA_FONT_PACK_PIN
};

[[nodiscard]] constexpr const char* font_pack_error_code_name(FontPackErrorCode code) {
    return code == FontPackErrorCode::Pin ? "TESSERA_FONT_PACK_PIN" : "TESSERA_FONT_PACK_MANIFEST";
}

struct FontPackError {
    FontPackErrorCode code = FontPackErrorCode::Manifest;
    std::string message;
};

// Build-time parse of `font-pack.json`; never throws. Rejects a malformed id, version, hash or
// weight, an empty font list and a repeated file.
[[nodiscard]] std::expected<FontPack, FontPackError> parse_font_pack(std::string_view json);

// One `fontPacks` entry of a theme asset: the pack version the theme is pinned to.
struct ThemeFontPin {
    std::string theme;
    std::string pack_id;
    std::string pack_version;
};

// Reads the optional top-level `fontPacks` object of a theme asset (theme id to
// `{ "id": ..., "version": ... }`). The theme asset parser ignores the key, so a pack-less app
// pays nothing for it.
[[nodiscard]] std::expected<std::vector<ThemeFontPin>, FontPackError>
read_theme_font_pins(std::string_view theme_asset_json);

// A pinned theme never changes look, so the pin is an exact version, not a range: every pin must
// name an existing, versioned theme of `asset`, the pack's id, and exactly the pack's version.
// Returns the first violation in `pins` order.
[[nodiscard]] std::optional<FontPackError> check_theme_font_pins(const ThemeAsset& asset,
                                                                 std::span<const ThemeFontPin> pins,
                                                                 const FontPack& pack);

} // namespace tessera::ui::style
