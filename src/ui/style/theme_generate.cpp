// SPDX-License-Identifier: MIT

// Instrument token-set generation from one seed color (issue #79). The derivation and the contrast
// check that gates it; the OKLCH solve lives in theme_solve.cpp and the JSON writer in
// theme_generate_json.cpp.
//
// Derivation, in the order it runs (docs/specs/theme-generation-v1.md is normative):
//   1. Read the seed as sRGB and convert to OKLCH. An achromatic seed (chroma < 0.004) takes the
//      Instrument accent hue, because a grey has no hue to carry.
//   2. Build the accent tonal scale: eleven fixed lightness steps, with a chroma profile that peaks
//      near L 0.62 and tapers at the ends, each step clamped into the sRGB gamut. Steps are
//      quantized to 8-bit sRGB before any measurement.
//   3. Per variant, build the three surfaces as neutrals at the seed's hue with a small chroma.
//   4. Solve each role against the contrast floors of contrast.hpp by walking OKLCH lightness
//      outward from the role's target: --on-surface and --on-surface-muted must clear the normal
//      text floor on all three surfaces; --accent takes the scale step nearest its target that
//      clears the text floor on its own ink and the non-text floor on every surface; --focus-ring,
//      --border-strong, --danger, --success and --warning solve the same way. Every solve stops at
//      1.2% above the floor so a later rounding cannot flip a passing pair.
//   5. Quantize every role to 8-bit sRGB, assemble the tokens, and run `check_theme_contrast` over
//      the result. A finding is an error, never a warning.

#include "tessera/ui/style/color_oklch.hpp"
#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"
#include "theme_generate_internal.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

using detail::clears_surfaces;
using detail::color_at;
using detail::instrument_scale_steps;
using detail::instrument_semantic_hues;
using detail::instrument_tuning;
using detail::InstrumentTuning;
using detail::nearest_scale_step;
using detail::SemanticHue;
using detail::solve_lightness;
using detail::SolveSurfaces;

std::unexpected<ThemeGenerateError> error(ThemeGenerateErrorCode code, std::string message) {
    return std::unexpected(ThemeGenerateError{code, std::move(message)});
}

bool is_theme_id(std::string_view id) {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') {
        return false;
    }
    return std::all_of(id.begin() + 1, id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

void add_token(Theme& theme, std::string_view name, std::string value) {
    theme.tokens.push_back(ThemeToken{std::string(name), std::move(value)});
}

void add_color_token(Theme& theme, std::string_view name, const ColorRgba& color) {
    add_token(theme, name, to_hex(color));
}

} // namespace

const std::vector<ThemeVariantSpec>& instrument_variant_specs() {
    static const std::vector<ThemeVariantSpec> specs = {
        ThemeVariantSpec{"light", false, 1.000, 0.972, 0.940, 0.010, 0.22, 0.46, 0.55, 0.50, 0.50,
                         0.86, ColorRgba{1.0f, 1.0f, 1.0f, 1.0f}},
        ThemeVariantSpec{"dark", false, 0.215, 0.255, 0.150, 0.014, 0.95, 0.76, 0.72, 0.72, 0.76,
                         0.34, ColorRgba{0.0f, 0.0f, 0.0f, 1.0f}},
        ThemeVariantSpec{"high-contrast", true, 1.000, 1.000, 0.955, 0.000, 0.00, 0.28, 0.38, 0.36,
                         0.36, 0.00, ColorRgba{1.0f, 1.0f, 1.0f, 1.0f}},
    };
    return specs;
}

std::span<const AccentScaleStep> instrument_accent_scale() {
    return instrument_scale_steps();
}

std::expected<ThemeAsset, ThemeGenerateError>
generate_theme_asset(std::string_view seed, const ThemeGenerateOptions& options,
                     std::span<const ThemeVariantSpec> specs) {
    if (specs.empty()) {
        return error(ThemeGenerateErrorCode::InvalidPrefix, "at least one variant spec is needed");
    }
    const std::optional<OklchColor> seed_oklch = parse_oklch(seed);
    if (!seed_oklch) {
        return error(ThemeGenerateErrorCode::InvalidSeed,
                     "seed `" + std::string(seed) + "` is not a color");
    }
    // The prefix is the free part of a theme id, so it is the part that can be invalid: every id
    // built from it is `<prefix>-<suffix>`, and the suffix table is fixed.
    if (!is_theme_id(options.id_prefix)) {
        return error(ThemeGenerateErrorCode::InvalidPrefix,
                     "id prefix `" + options.id_prefix + "` must match [a-z][a-z0-9-]*");
    }
    for (const ThemeVariantSpec& spec : specs) {
        if (!is_theme_id(spec.suffix)) {
            return error(ThemeGenerateErrorCode::InvalidPrefix, "variant suffix `" +
                                                                    std::string(spec.suffix) +
                                                                    "` must match [a-z][a-z0-9-]*");
        }
    }
    if (!options.version.empty() && !is_theme_version(options.version)) {
        return error(ThemeGenerateErrorCode::InvalidVersion,
                     "version `" + options.version + "` must be MAJOR.MINOR.PATCH");
    }

    const InstrumentTuning& tuning = instrument_tuning();
    const double hue =
        seed_oklch->chroma >= tuning.achromatic_threshold ? seed_oklch->hue : tuning.instrument_hue;
    const double accent_chroma =
        std::clamp(seed_oklch->chroma, tuning.accent_chroma_min, tuning.accent_chroma_max);
    const std::span<const AccentScaleStep> scale = instrument_scale_steps();
    std::vector<ColorRgba> accent_scale;
    accent_scale.reserve(scale.size());
    for (const AccentScaleStep& step : scale) {
        accent_scale.push_back(
            color_at(step.lightness, detail::chroma_of_step(step.lightness, accent_chroma), hue));
    }

    ThemeAsset asset;
    asset.schema_version = 1;
    asset.start = options.id_prefix + "-" + std::string(specs.front().suffix);
    asset.themes.reserve(specs.size());
    for (const ThemeVariantSpec& spec : specs) {
        Theme theme;
        theme.id = options.id_prefix + "-" + std::string(spec.suffix);
        theme.version = options.version;

        const SolveSurfaces surfaces = detail::solve_surfaces(spec, hue);
        const auto text_ok = [&surfaces](const ColorRgba& c) {
            return clears_surfaces(c, surfaces, surfaces.text_floor);
        };
        // A fill must carry its ink at the text floor and stand out from every surface at the
        // non-text floor: the same color is used as a button background and as an indicator.
        const auto fill_ok = [&surfaces, &spec](const ColorRgba& c) {
            return contrast_ratio(spec.ink_on_fill, c) >= surfaces.text_floor &&
                   clears_surfaces(c, surfaces, surfaces.indicator_floor);
        };

        const std::optional<ColorRgba> on_surface =
            solve_lightness(spec.on_surface_lightness, spec.neutral_chroma, hue, text_ok);
        const std::optional<ColorRgba> muted =
            solve_lightness(spec.muted_lightness, spec.neutral_chroma, hue, text_ok);
        const std::optional<ColorRgba> focus_ring = solve_lightness(
            spec.focus_ring_lightness, accent_chroma, hue, [&surfaces](const ColorRgba& c) {
                return clears_surfaces(c, surfaces, surfaces.indicator_floor);
            });
        const std::optional<ColorRgba> border_strong =
            solve_lightness(0.55, spec.neutral_chroma, hue, [&surfaces](const ColorRgba& c) {
                return clears_surfaces(c, surfaces, surfaces.indicator_floor);
            });
        if (!on_surface || !muted || !focus_ring || !border_strong) {
            return error(ThemeGenerateErrorCode::Unsolvable,
                         "no lightness reaches the contrast floor of theme `" + theme.id + "`");
        }

        std::optional<ColorRgba> accent;
        if (const std::optional<std::size_t> step = nearest_scale_step(
                spec.accent_lightness, [&accent_scale, &fill_ok](std::size_t index) {
                    return fill_ok(accent_scale[index]);
                })) {
            accent = accent_scale[*step];
        } else {
            accent = solve_lightness(spec.accent_lightness, accent_chroma, hue, fill_ok);
        }
        if (!accent) {
            return error(ThemeGenerateErrorCode::Unsolvable,
                         "no accent step reaches the contrast floor of theme `" + theme.id + "`");
        }

        const std::span<const SemanticHue> semantic_hues = instrument_semantic_hues();
        std::vector<ColorRgba> semantic;
        semantic.reserve(semantic_hues.size());
        for (const SemanticHue& entry : semantic_hues) {
            const std::optional<ColorRgba> fill =
                solve_lightness(spec.semantic_lightness, entry.chroma, entry.hue, fill_ok);
            if (!fill) {
                return error(ThemeGenerateErrorCode::Unsolvable,
                             "no lightness reaches the contrast floor of `--" +
                                 std::string(entry.role) + "` in theme `" + theme.id + "`");
            }
            semantic.push_back(*fill);
        }

        // Token order is the published Instrument order: surfaces and text first, then the accent
        // family, then status, then the reserved chrome pair, then the static design tokens.
        add_color_token(theme, "--surface", surfaces.colors[0]);
        add_color_token(theme, "--surface-raised", surfaces.colors[1]);
        add_color_token(theme, "--surface-sunken", surfaces.colors[2]);
        add_color_token(theme, "--on-surface", *on_surface);
        add_color_token(theme, "--on-surface-muted", *muted);
        add_color_token(theme, "--border",
                        color_at(spec.border_lightness, spec.neutral_chroma, hue));
        add_color_token(theme, "--border-strong", *border_strong);
        add_color_token(theme, "--accent", *accent);
        add_color_token(theme, "--on-accent", spec.ink_on_fill);
        if (options.include_accent_scale) {
            for (std::size_t i = 0; i < accent_scale.size(); ++i) {
                add_color_token(theme, "--accent-" + std::to_string(scale[i].name),
                                accent_scale[i]);
            }
        }
        for (std::size_t i = 0; i < semantic.size(); ++i) {
            const std::string role = "--" + std::string(semantic_hues[i].role);
            add_color_token(theme, role, semantic[i]);
            add_color_token(theme, "--on-" + std::string(semantic_hues[i].role), spec.ink_on_fill);
        }
        add_color_token(theme, "--focus-ring", *focus_ring);
        // The reserved chrome pair (stylesheet-v1.md 9.2) follows the page surface and its ink, so
        // the pre-first-frame window and the loading screen match the app.
        add_color_token(theme, kChromeBackgroundToken, surfaces.colors[0]);
        add_color_token(theme, kChromeForegroundToken, *on_surface);
        detail::append_static_tokens(theme, spec);

        asset.themes.push_back(std::move(theme));
    }

    // The gate: a generated asset that misses a floor is an error, not a warning, so nothing
    // downstream can write a token set this module has not verified. The high-contrast variant is
    // the one that opts into the stricter floors, by suffix rather than by position.
    std::string high_contrast_id;
    for (const ThemeVariantSpec& spec : specs) {
        if (spec.high_contrast) {
            high_contrast_id = options.id_prefix + "-" + std::string(spec.suffix);
        }
    }
    const std::vector<ContrastFinding> findings = check_theme_contrast(asset, high_contrast_id);
    if (!findings.empty()) {
        return error(ThemeGenerateErrorCode::Contrast, describe_contrast_finding(findings.front()));
    }
    return asset;
}

std::expected<ThemeAsset, ThemeGenerateError>
generate_instrument_theme(std::string_view seed, const ThemeGenerateOptions& options) {
    return generate_theme_asset(seed, options, instrument_variant_specs());
}

} // namespace tessera::ui::style
