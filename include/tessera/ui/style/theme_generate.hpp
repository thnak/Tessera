// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Instrument token-set generation from one seed color (issue #79, ADR-047 item 5).
//
// One accent color in, a complete `assets.theme` asset out: three themes (`instrument-light`,
// `instrument-dark`, `instrument-high-contrast`) sharing one token name set, every text and
// indicator pair cleared against the WCAG 2.2 floors of `contrast.hpp`. The derivation runs in
// OKLCH so the accent scale steps evenly and the neutral greys carry a trace of the seed's hue.
// Normative rules and the role table: docs/specs/theme-generation-v1.md.
//
// The same function serves the `tessera theme --seed <color>` CLI and, later, the designer's
// theme picker: the CLI is a thin wrapper that writes `format_theme_asset` to disk.
namespace tessera::ui::style {

// One theme variant's derivation inputs, in OKLCH lightness (0..1). The numbers are a design
// choice, documented in the spec; they are public so a tool (and a test) can see and vary the
// design without re-deriving it.
struct ThemeVariantSpec {
    std::string_view suffix; // theme id suffix: the id is `<options.id_prefix>-<suffix>`
    bool high_contrast;      // takes the stricter WCAG floors (7:1 text, 4.5:1 indicator)
    double surface;          // OKLCH L of --surface
    double surface_raised;
    double surface_sunken;
    double neutral_chroma;       // chroma of every neutral; the seed's hue is kept
    double on_surface_lightness; // target L of --on-surface (solved upward if it falls short)
    double muted_lightness;      // target L of --on-surface-muted
    double accent_lightness;     // target scale step for --accent
    double semantic_lightness;   // target L of --danger/--success/--warning
    double focus_ring_lightness; // target L of --focus-ring
    double border_lightness;     // L of --border (a hairline, not a 3:1 boundary)
    // The text color on a filled control. Fixed per variant rather than picked by contrast, so a
    // mid-lightness accent cannot flip polarity between seeds: light and high contrast use white
    // ink, dark uses black ink.
    ColorRgba ink_on_fill;
};

// The Instrument design's variant table: three variants in asset order, with the `light`, `dark`
// and `high-contrast` suffixes. `generate_theme_asset` combines them with `options.id_prefix`.
[[nodiscard]] const std::vector<ThemeVariantSpec>& instrument_variant_specs();

// The tonal scale every generated asset carries: step name and OKLCH lightness. The accent role is
// one of these steps, so app code can step the same scale for hover/pressed states.
struct AccentScaleStep {
    int name; // 50, 100, ... 950
    double lightness;
};

[[nodiscard]] std::span<const AccentScaleStep> instrument_accent_scale();

struct ThemeGenerateOptions {
    // Theme ids become `<prefix>-light`, `<prefix>-dark`, `<prefix>-high-contrast`. The prefix is a
    // theme-id fragment, so it must match `[a-z][a-z0-9-]*` and leave room for the suffix.
    std::string id_prefix = "instrument";
    // `themeVersions` entry for every theme. Empty writes an unversioned asset.
    std::string version = "1.0.0";
    // Emit `--accent-<step>` for every step of `instrument_accent_scale()`.
    bool include_accent_scale = true;
};

enum class ThemeGenerateErrorCode : std::uint8_t {
    InvalidSeed,   // not a color `parse_color` accepts
    InvalidPrefix, // id prefix does not form valid theme ids
    InvalidVersion,
    Unsolvable, // a role could not reach its floor anywhere in the lightness range
    Contrast,   // the derived asset still misses a floor: reported instead of written
};

[[nodiscard]] constexpr const char* theme_generate_error_code_name(ThemeGenerateErrorCode code) {
    switch (code) {
    case ThemeGenerateErrorCode::InvalidSeed:
        return "TESSERA_THEME_SEED";
    case ThemeGenerateErrorCode::InvalidPrefix:
        return "TESSERA_THEME_ID_PREFIX";
    case ThemeGenerateErrorCode::InvalidVersion:
        return "TESSERA_THEME_VERSION";
    case ThemeGenerateErrorCode::Unsolvable:
        return "TESSERA_THEME_UNSOLVABLE";
    case ThemeGenerateErrorCode::Contrast:
        return "TESSERA_THEME_CONTRAST";
    }
    return "TESSERA_THEME_SEED";
}

struct ThemeGenerateError {
    ThemeGenerateErrorCode code = ThemeGenerateErrorCode::InvalidSeed;
    std::string message;
};

// Derives the asset from `seed` (any CSS color `parse_color` accepts: `#rgb`, `#rrggbb`, `rgb()`,
// a named color) with `specs`. Deterministic: the same seed and specs always produce the same
// asset, and `format_theme_asset` always produces the same bytes. Every color is quantized to 8-bit
// sRGB before any contrast check, so the checker measures what the app paints.
//
// Fails with `Contrast` if the derived asset misses a floor, which means a spec in `specs` is not
// solvable — the generator never writes a token set it has not checked.
[[nodiscard]] std::expected<ThemeAsset, ThemeGenerateError>
generate_theme_asset(std::string_view seed, const ThemeGenerateOptions& options,
                     std::span<const ThemeVariantSpec> specs);

// `generate_theme_asset` with the Instrument table.
[[nodiscard]] std::expected<ThemeAsset, ThemeGenerateError>
generate_instrument_theme(std::string_view seed, const ThemeGenerateOptions& options = {});

// Deterministic JSON of an asset in `assets.theme` shape (stylesheet-v1.md section 9.1): two-space
// indent, members in `schemaVersion`, `start`, `themeVersions`, `themes` order, themes in asset
// order, and within a theme the tokens in asset order with one `name: value` pair per line. No
// floating point is ever written (token values are strings), so there is no formatting drift.
//
// The implicit built-in `default` theme is not emitted: it exists only after parsing, and a file
// that declared it would override the built-in chrome pair.
[[nodiscard]] std::string format_theme_asset(const ThemeAsset& asset);

} // namespace tessera::ui::style
