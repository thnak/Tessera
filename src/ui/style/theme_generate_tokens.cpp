// SPDX-License-Identifier: MIT

// The Instrument design data: the tuning constants of the derivation, the status hues, the accent
// tonal scale, and the static (non-color) token table. They are split from the color derivation
// because they are design decisions rather than algorithm: they carry no contrast obligation of
// their own and change only with the design language, not with the seed.

#include "theme_generate_internal.hpp"

#include <array>
#include <string>
#include <string_view>

namespace tessera::ui::style::detail {
namespace {

struct StaticToken {
    std::string_view name;
    std::string_view light;
    std::string_view dark;
    std::string_view high_contrast;
};

// `--border-width` doubles in high contrast, and the floating shadow is heavier on a dark surface
// so it stays visible; every other value is identical in all three variants.
constexpr std::array<StaticToken, 34> kStaticTokens{{
    {"--space-1", "4px", "4px", "4px"},
    {"--space-2", "8px", "8px", "8px"},
    {"--space-3", "12px", "12px", "12px"},
    {"--space-4", "16px", "16px", "16px"},
    {"--space-6", "24px", "24px", "24px"},
    {"--space-8", "32px", "32px", "32px"},
    {"--radius-sm", "2px", "2px", "2px"},
    {"--radius-md", "4px", "4px", "4px"},
    {"--radius-lg", "8px", "8px", "8px"},
    {"--border-width", "1px", "1px", "2px"},
    {"--font-sans", "\"IBM Plex Sans\", sans-serif", "\"IBM Plex Sans\", sans-serif",
     "\"IBM Plex Sans\", sans-serif"},
    {"--font-mono", "\"IBM Plex Mono\", monospace", "\"IBM Plex Mono\", monospace",
     "\"IBM Plex Mono\", monospace"},
    {"--font-size-label", "11px", "11px", "11px"},
    {"--font-size-body", "14px", "14px", "14px"},
    {"--font-size-title", "18px", "18px", "18px"},
    {"--line-height-label", "16px", "16px", "16px"},
    {"--line-height-body", "20px", "20px", "20px"},
    {"--line-height-title", "24px", "24px", "24px"},
    {"--motion-fast", "120ms", "120ms", "120ms"},
    {"--motion-normal", "200ms", "200ms", "200ms"},
    {"--motion-slow", "320ms", "320ms", "320ms"},
    {"--breakpoint-sm", "480px", "480px", "480px"},
    {"--breakpoint-md", "768px", "768px", "768px"},
    {"--breakpoint-lg", "1200px", "1200px", "1200px"},
    {"--shadow-floating-y", "4px", "4px", "4px"},
    {"--shadow-floating-blur", "12px", "12px", "12px"},
    {"--z-dropdown", "100", "100", "100"},
    {"--z-overlay", "200", "200", "200"},
    {"--z-modal", "300", "300", "300"},
    {"--z-toast", "400", "400", "400"},
    {"--control-height", "32px", "32px", "32px"},
    {"--control-height-compact", "24px", "24px", "24px"},
    {"--control-padding-x", "12px", "12px", "12px"},
    {"--control-padding-x-compact", "8px", "8px", "8px"},
}};

std::string_view variant_value(const StaticToken& token, const ThemeVariantSpec& spec) {
    if (spec.high_contrast) {
        return token.high_contrast;
    }
    return spec.ink_on_fill.r < 0.5f ? token.dark : token.light;
}

// The accent tonal scale: eleven even lightness steps. Each step's chroma comes from the tuning
// profile, so the pale and dark ends do not flatten to grey against the sRGB boundary.
constexpr std::array<AccentScaleStep, 11> kAccentScale{{
    {50, 0.970},
    {100, 0.930},
    {200, 0.880},
    {300, 0.820},
    {400, 0.750},
    {500, 0.670},
    {600, 0.580},
    {700, 0.500},
    {800, 0.420},
    {900, 0.340},
    {950, 0.260},
}};

constexpr std::array<SemanticHue, 3> kSemanticHues{{
    {"danger", 25.0, 0.160},
    {"success", 148.0, 0.130},
    {"warning", 85.0, 0.140},
}};

} // namespace

const InstrumentTuning& instrument_tuning() {
    static constexpr InstrumentTuning tuning{};
    return tuning;
}

std::span<const SemanticHue> instrument_semantic_hues() {
    return kSemanticHues;
}

std::span<const AccentScaleStep> instrument_scale_steps() {
    return kAccentScale;
}

void append_static_tokens(Theme& theme, const ThemeVariantSpec& spec) {
    for (const StaticToken& token : kStaticTokens) {
        theme.tokens.push_back(
            ThemeToken{std::string(token.name), std::string(variant_value(token, spec))});
    }
    theme.tokens.push_back(ThemeToken{
        "--shadow-floating-color",
        spec.high_contrast
            ? "rgba(0, 0, 0, 0.18)"
            : (spec.ink_on_fill.r < 0.5f ? "rgba(0, 0, 0, 0.45)" : "rgba(0, 0, 0, 0.18)")});
}

} // namespace tessera::ui::style::detail
