// SPDX-License-Identifier: MIT

// The token-set generator across the hue wheel (issue #79 task 2). Every seed — a full hue sweep at
// three lightness levels, the achromatic extremes, and the hard accents the issue names (saturated
// orange, yellow, pure blue) — must produce:
//   * three themes with an identical token name set in the same order,
//   * every contrast pair of the Instrument role table above its floor,
//   * the reserved chrome pair and the required semantic roles,
//   * byte-identical JSON for the same seed, and different JSON for a different seed,
//   * a working `--prefix`/`--version`/`--no-accent-scale` option surface.
// The contrast check runs through the shipped checker, not a local copy, so a role cannot be
// solved against one floor and asserted against another.

#include "tessera/ui/style/color_oklch.hpp"
#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "theme_generate: " << message << "\n";
        ++failures;
    }
}

std::string hex_of(const style::ColorRgba& color) {
    return style::to_hex(color);
}

// A seed for OKLCH (L, C, H), written as the hex a user would type.
std::string seed_at(double lightness, double chroma, double hue) {
    style::OklchColor color;
    color.lightness = lightness;
    color.chroma = chroma;
    color.hue = hue;
    return hex_of(style::quantize_to_srgb8(color));
}

// Seeds with a hue to carry: every distinct seed must produce a distinct document.
std::vector<std::string> chromatic_seeds() {
    std::vector<std::string> seeds;
    for (int degrees = 0; degrees < 360; degrees += 10) {
        seeds.push_back(seed_at(0.62, 0.19, static_cast<double>(degrees)));
    }
    for (int degrees = 0; degrees < 360; degrees += 30) {
        seeds.push_back(seed_at(0.80, 0.14, static_cast<double>(degrees)));
        seeds.push_back(seed_at(0.45, 0.09, static_cast<double>(degrees)));
    }
    // The hard seeds the issue names, plus the shipped Instrument accent. A hard seed can already
    // be a point of the sweep above, so the append keeps the list duplicate-free: two names for one
    // seed would otherwise look like a determinism failure.
    const auto add = [&seeds](std::string seed) {
        if (std::find(seeds.begin(), seeds.end(), seed) == seeds.end()) {
            seeds.push_back(std::move(seed));
        }
    };
    for (const std::string_view seed : {"#ff6a00", "#ffe000", "#0000ff", "#ff0000", "#00ff00",
                                        "#7f00ff", "#0b5cad", "#123456", "#00a88b", "#db4781"}) {
        add(std::string(seed));
    }
    return seeds;
}

// Seeds with no hue. They all fall back to the Instrument hue, so they generate one identical
// document: that is the documented fallback, not a bug.
constexpr std::string_view kAchromaticSeeds[]{"#ffffff", "#000000", "#808080", "#c0c0c0"};

std::vector<std::string> seed_sweep() {
    std::vector<std::string> seeds = chromatic_seeds();
    for (const std::string_view seed : kAchromaticSeeds) {
        seeds.emplace_back(seed);
    }
    return seeds;
}

// The roles every generated variant must define, beyond the pair table.
constexpr std::string_view kRequiredTokens[]{
    "--surface",          "--surface-raised", "--surface-sunken",    "--on-surface",
    "--on-surface-muted", "--border",         "--border-strong",     "--accent",
    "--on-accent",        "--accent-500",     "--accent-950",        "--danger",
    "--on-danger",        "--success",        "--on-success",        "--warning",
    "--on-warning",       "--focus-ring",     "--window-background", "--window-foreground",
    "--space-4",          "--radius-md",      "--border-width",
};

void check_variant(const style::ThemeAsset& asset, const style::Theme& theme) {
    const std::string label = theme.id + " (" + asset.start + ")";
    check(theme.version == "1.0.0", label + " carries the theme version");
    for (const std::string_view name : kRequiredTokens) {
        check(theme.find(name) != nullptr, label + " defines " + std::string(name));
    }
    // Every token is a color or a length/string the theme asset parser accepts; re-parsing the
    // generated text is the golden test's job, but the values must at least be non-empty.
    for (const style::ThemeToken& token : theme.tokens) {
        check(!token.value.empty(), label + " " + token.name + " has a value");
        check(token.name.starts_with("--"), label + " " + token.name + " is a custom property");
    }
    // Spacing stays on the 4 px grid and radii in the 2 to 8 px range (ADR-047 item 5).
    for (const std::string_view space :
         {"--space-1", "--space-2", "--space-3", "--space-4", "--space-6", "--space-8"}) {
        const style::ThemeToken* token = theme.find(space);
        const int px = token != nullptr ? std::atoi(token->value.c_str()) : -1;
        check(px > 0 && px % 4 == 0, label + " " + std::string(space) + " is on the 4 px grid");
    }
    for (const std::string_view radius : {"--radius-sm", "--radius-md", "--radius-lg"}) {
        const style::ThemeToken* token = theme.find(radius);
        const int px = token != nullptr ? std::atoi(token->value.c_str()) : -1;
        check(px >= 2 && px <= 8, label + " " + std::string(radius) + " is 2 to 8 px");
    }
}

} // namespace

int main() {
    const std::vector<std::string> seeds = seed_sweep();
    check(seeds.size() >= 60,
          "the sweep covers the hue wheel (" + std::to_string(seeds.size()) + " seeds)");

    std::vector<std::string> documents;
    for (const std::string& seed : seeds) {
        const auto asset = style::generate_instrument_theme(seed);
        if (!asset) {
            check(false, "seed " + seed + " failed: " +
                             style::theme_generate_error_code_name(asset.error().code) + ": " +
                             asset.error().message);
            continue;
        }
        // Three themes, in order, with the Instrument ids.
        check(asset->themes.size() == 3, "seed " + seed + " has three themes");
        if (asset->themes.size() == 3) {
            check(asset->themes[0].id == "instrument-light", "seed " + seed + " light id");
            check(asset->themes[1].id == "instrument-dark", "seed " + seed + " dark id");
            check(asset->themes[2].id == "instrument-high-contrast",
                  "seed " + seed + " high-contrast id");
            check(asset->start == "instrument-light", "seed " + seed + " starts on light");
        }
        // An identical token name set, in the same order, in all three variants: a component that
        // reads a token in one variant must find it in the other two.
        for (std::size_t i = 1; i < asset->themes.size(); ++i) {
            const style::Theme& first = asset->themes.front();
            const style::Theme& other = asset->themes[i];
            check(first.tokens.size() == other.tokens.size(),
                  "seed " + seed + ": " + other.id + " has the same token count as light");
            for (std::size_t t = 0; t < std::min(first.tokens.size(), other.tokens.size()); ++t) {
                check(first.tokens[t].name == other.tokens[t].name,
                      "seed " + seed + ": " + other.id + " token order matches at " +
                          first.tokens[t].name);
            }
        }
        for (const style::Theme& theme : asset->themes) {
            check_variant(*asset, theme);
        }
        // The generator ran the shipped checker over its own output, so this must be empty.
        const std::vector<style::ContrastFinding> findings =
            style::check_theme_contrast(*asset, "instrument-high-contrast");
        for (const style::ContrastFinding& finding : findings) {
            check(false, "seed " + seed + ": " + style::describe_contrast_finding(finding));
        }
        documents.push_back(style::format_theme_asset(*asset));
    }

    // Determinism: the same seed, generated twice, produces identical bytes; a different seed does
    // not (so the sweep is not accidentally emitting one constant document).
    const auto first = style::generate_instrument_theme("#0b5cad");
    const auto again = style::generate_instrument_theme("#0b5cad");
    check(first.has_value() && again.has_value(), "the determinism seeds generate");
    if (first && again) {
        check(style::format_theme_asset(*first) == style::format_theme_asset(*again),
              "the same seed produces identical bytes");
    }
    std::vector<std::string> distinct;
    for (const std::string& seed : chromatic_seeds()) {
        const auto asset = style::generate_instrument_theme(seed);
        if (asset) {
            distinct.push_back(style::format_theme_asset(*asset));
        }
    }
    check(distinct.size() == chromatic_seeds().size(),
          "every chromatic seed generates for the determinism sweep");
    std::sort(distinct.begin(), distinct.end());
    check(std::adjacent_find(distinct.begin(), distinct.end()) == distinct.end(),
          "every distinct chromatic seed produces a distinct document");
    check(distinct.size() >= 60, "the determinism sweep covers the hue wheel");

    // The generated document round-trips through the theme asset parser with no token-missing
    // warnings, so a written token set is a valid `assets.theme` file.
    if (first) {
        const std::string text = style::format_theme_asset(*first);
        const auto parsed = style::parse_theme_asset(text);
        check(parsed.has_value(), "the generated JSON parses as a theme asset");
        if (parsed) {
            check(parsed->warnings.empty(), "the generated JSON has no token-missing warning");
            check(parsed->themes.size() == 4,
                  "the parsed asset holds the built-in default plus three themes");
            check(parsed->start == "instrument-light", "the start theme survives the round trip");
            check(style::format_theme_asset(*parsed) == text,
                  "formatting the parsed asset reproduces the same bytes");
        }
    }

    // The option surface: a custom prefix, a custom version, no accent scale.
    style::ThemeGenerateOptions options;
    options.id_prefix = "acme";
    options.version = "2.4.0";
    options.include_accent_scale = false;
    const auto custom = style::generate_instrument_theme("#ff6a00", options);
    check(custom.has_value(), "a custom prefix generates");
    if (custom) {
        check(custom->themes.size() == 3, "the custom prefix still has three themes");
        check(custom->themes[0].id == "acme-light", "the prefix names the light theme");
        check(custom->themes[1].id == "acme-dark", "the prefix names the dark theme");
        check(custom->themes[2].id == "acme-high-contrast",
              "the prefix names the high-contrast theme");
        check(custom->themes[0].version == "2.4.0", "the version is stamped");
        check(custom->themes[0].find("--accent-500") == nullptr,
              "no accent scale steps were emitted");
        check(style::check_theme_contrast(*custom, "acme-high-contrast").empty(),
              "a custom prefix still clears every floor");
    }

    // An unversioned asset writes no `themeVersions`, and the parser accepts it.
    options.version = "";
    const auto unversioned = style::generate_instrument_theme("#0b5cad", options);
    check(unversioned.has_value(), "an unversioned asset generates");
    if (unversioned) {
        const std::string text = style::format_theme_asset(*unversioned);
        check(text.find("themeVersions") == std::string::npos, "no themeVersions when unversioned");
        check(style::parse_theme_asset(text).has_value(), "an unversioned asset parses");
    }

    // Errors: a non-color seed, an id prefix that cannot form theme ids, a malformed version.
    const auto bad_seed = style::generate_instrument_theme("not-a-color");
    check(!bad_seed.has_value(), "a non-color seed is rejected");
    if (!bad_seed) {
        check(bad_seed.error().code == style::ThemeGenerateErrorCode::InvalidSeed,
              "the seed error is TESSERA_THEME_SEED");
    }
    style::ThemeGenerateOptions bad_prefix;
    bad_prefix.id_prefix = "Bad Prefix";
    const auto bad_prefix_asset = style::generate_instrument_theme("#0b5cad", bad_prefix);
    check(!bad_prefix_asset.has_value(), "an invalid id prefix is rejected");
    if (!bad_prefix_asset) {
        check(bad_prefix_asset.error().code == style::ThemeGenerateErrorCode::InvalidPrefix,
              "the prefix error is TESSERA_THEME_ID_PREFIX");
    }
    style::ThemeGenerateOptions bad_version;
    bad_version.version = "1.0";
    const auto bad_version_asset = style::generate_instrument_theme("#0b5cad", bad_version);
    check(!bad_version_asset.has_value(), "a malformed version is rejected");
    if (!bad_version_asset) {
        check(bad_version_asset.error().code == style::ThemeGenerateErrorCode::InvalidVersion,
              "the version error is TESSERA_THEME_VERSION");
    }

    // A seed with no chroma falls back to the Instrument hue rather than emitting a grey accent.
    const auto grey_seed = style::generate_instrument_theme("#808080");
    check(grey_seed.has_value(), "an achromatic seed generates");
    if (grey_seed) {
        const style::ThemeToken* accent = grey_seed->themes[0].find("--accent");
        check(accent != nullptr, "the achromatic seed still has an accent");
        if (accent != nullptr) {
            const style::ColorParseResult parsed = style::parse_color(accent->value);
            check(parsed.ok, "the accent is a parseable color");
            const style::OklchColor accent_oklch = style::to_oklch(parsed.color);
            check(accent_oklch.chroma > 0.03,
                  "an achromatic seed still gets a saturated accent (chroma " +
                      std::to_string(accent_oklch.chroma) + ")");
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "theme_generate: ok (" << seeds.size() << " seeds)\n";
    return 0;
}
