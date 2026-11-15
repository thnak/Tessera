// SPDX-License-Identifier: MIT

// WCAG 2.2 contrast for theme tokens (issue #79). One implementation, used three ways: the
// generator solves against the same table the checker lints with, so a role cannot be generated
// against one floor and reported against another.

#include "tessera/ui/style/color_oklch.hpp"
#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {
namespace {

struct PairRow {
    std::string_view foreground;
    std::string_view background;
    ContrastRequirement requirement;
};

// The Instrument contrast contract (ADR-047 item 5, component-library.md section 2). The three
// surfaces are the three backgrounds a role can sit on; a role is checked against all of them, not
// only `--surface`, because a control on a raised panel must be as legible as one on the page.
//
// Roles and their thresholds:
//   normal text (4.5:1, 7:1 high contrast)  every `--on-*` role — on-surface, on-surface-muted,
//                                          on-accent, on-danger, on-success, on-warning — plus
//                                          the reserved chrome pair. These are the roles a
//                                          component uses for body, label and value text at any
//                                          size, so they take the strictest text floor.
//   large text  (3:1, 4.5:1 high contrast)  `--accent` used as text. ADR-047 item 5 forbids the
//                                          accent as the only carrier of small text, so accent
//                                          text is headings and other large type; it is cited
//                                          once per surface and that 3:1 also covers the same
//                                          pair as a fill and as a boundary, so the pair is not
//                                          listed a second time under non-text.
//   non-text    (3:1, 4.5:1 high contrast)  focus ring, strong border and the three status colors
//                                          where they are used as fills and indicators
//                                          (WCAG 2.2 SC 1.4.11).
// `--border` is deliberately absent: it is a hairline divider, not a component boundary, and WCAG
// 1.4.11 does not require 3:1 for it (the shipped Instrument asset sits at 1.6:1 by design).
constexpr std::array<PairRow, 29> kPairTable{{
    // normal text on the three surfaces
    {"--on-surface", "--surface", ContrastRequirement::NormalText},
    {"--on-surface", "--surface-raised", ContrastRequirement::NormalText},
    {"--on-surface", "--surface-sunken", ContrastRequirement::NormalText},
    {"--on-surface-muted", "--surface", ContrastRequirement::NormalText},
    {"--on-surface-muted", "--surface-raised", ContrastRequirement::NormalText},
    {"--on-surface-muted", "--surface-sunken", ContrastRequirement::NormalText},
    // normal text on a filled control
    {"--on-accent", "--accent", ContrastRequirement::NormalText},
    {"--on-danger", "--danger", ContrastRequirement::NormalText},
    {"--on-success", "--success", ContrastRequirement::NormalText},
    {"--on-warning", "--warning", ContrastRequirement::NormalText},
    // the reserved chrome pair (stylesheet-v1.md 9.2)
    {"--window-foreground", "--window-background", ContrastRequirement::NormalText},
    // accent as large text
    {"--accent", "--surface", ContrastRequirement::LargeText},
    {"--accent", "--surface-raised", ContrastRequirement::LargeText},
    {"--accent", "--surface-sunken", ContrastRequirement::LargeText},
    // focus ring against the colors adjacent to it
    {"--focus-ring", "--surface", ContrastRequirement::NonText},
    {"--focus-ring", "--surface-raised", ContrastRequirement::NonText},
    {"--focus-ring", "--surface-sunken", ContrastRequirement::NonText},
    {"--border-strong", "--surface", ContrastRequirement::NonText},
    {"--border-strong", "--surface-raised", ContrastRequirement::NonText},
    {"--border-strong", "--surface-sunken", ContrastRequirement::NonText},
    {"--danger", "--surface", ContrastRequirement::NonText},
    {"--danger", "--surface-raised", ContrastRequirement::NonText},
    {"--danger", "--surface-sunken", ContrastRequirement::NonText},
    {"--success", "--surface", ContrastRequirement::NonText},
    {"--success", "--surface-raised", ContrastRequirement::NonText},
    {"--success", "--surface-sunken", ContrastRequirement::NonText},
    {"--warning", "--surface", ContrastRequirement::NonText},
    {"--warning", "--surface-raised", ContrastRequirement::NonText},
    {"--warning", "--surface-sunken", ContrastRequirement::NonText},
}};

std::optional<ColorRgba> token_color(const Theme& theme, std::string_view name) {
    const ThemeToken* token = theme.find(name);
    if (token == nullptr) {
        return std::nullopt;
    }
    const ColorParseResult parsed = parse_color(token->value);
    if (!parsed.ok) {
        return std::nullopt;
    }
    return parsed.color;
}

} // namespace

double contrast_floor(ContrastRequirement requirement, bool high_contrast) {
    switch (requirement) {
    case ContrastRequirement::NormalText:
        return high_contrast ? 7.0 : 4.5;
    case ContrastRequirement::LargeText:
        return high_contrast ? 4.5 : 3.0;
    case ContrastRequirement::NonText:
        return high_contrast ? 4.5 : 3.0;
    }
    return 4.5;
}

double relative_luminance(const ColorRgba& color) {
    return 0.2126 * srgb_to_linear(color.r) + 0.7152 * srgb_to_linear(color.g) +
           0.0722 * srgb_to_linear(color.b);
}

double contrast_ratio(const ColorRgba& a, const ColorRgba& b) {
    const double first = relative_luminance(a);
    const double second = relative_luminance(b);
    const double lighter = std::max(first, second);
    const double darker = std::min(first, second);
    return (lighter + 0.05) / (darker + 0.05);
}

const std::vector<ContrastPair>& instrument_contrast_pairs() {
    static const std::vector<ContrastPair> pairs = [] {
        std::vector<ContrastPair> rows;
        rows.reserve(kPairTable.size());
        for (const PairRow& row : kPairTable) {
            rows.push_back(ContrastPair{row.foreground, row.background, row.requirement});
        }
        return rows;
    }();
    return pairs;
}

std::vector<ContrastFinding> check_theme_contrast(const ThemeAsset& asset,
                                                  std::string_view high_contrast_theme) {
    std::vector<ContrastFinding> findings;
    for (const Theme& theme : asset.themes) {
        const bool high_contrast = theme.id == high_contrast_theme;
        // A theme that does not define `--surface` is not an Instrument token set: the built-in
        // `default` carries only the reserved chrome pair (stylesheet-v1.md 9.1/9.2), and a
        // project is free to ship such a theme. For those only the chrome pair is meaningful;
        // requiring the whole role table would make every chrome-only theme a false failure.
        const bool full_role_set = theme.find("--surface") != nullptr;
        for (const ContrastPair& pair : instrument_contrast_pairs()) {
            const bool chrome_pair = pair.foreground == kChromeForegroundToken;
            if (!full_role_set && !chrome_pair) {
                continue;
            }
            const double required = contrast_floor(pair.requirement, high_contrast);
            const std::string label =
                std::string(pair.foreground) + " on " + std::string(pair.background);
            const std::optional<ColorRgba> foreground = token_color(theme, pair.foreground);
            const std::optional<ColorRgba> background = token_color(theme, pair.background);
            if (!foreground || !background) {
                const std::string_view missing = !foreground ? pair.foreground : pair.background;
                findings.push_back(ContrastFinding{
                    theme.id, "missing or unparseable `" + std::string(missing) + "` in " + label,
                    0.0, required, pair.requirement});
                continue;
            }
            const double ratio = contrast_ratio(*foreground, *background);
            if (ratio < required) {
                findings.push_back(
                    ContrastFinding{theme.id, label, ratio, required, pair.requirement});
            }
        }
    }
    return findings;
}

std::string describe_contrast_finding(const ContrastFinding& finding) {
    char ratio[16] = {};
    char floor[16] = {};
    std::snprintf(ratio, sizeof(ratio), "%.2f", finding.ratio);
    std::snprintf(floor, sizeof(floor), "%.1f", finding.required);
    return "TESSERA_THEME_CONTRAST: theme `" + finding.theme + "`: " + finding.pair + " is " +
           ratio + ":1, below the " + floor + ":1 floor for " +
           contrast_requirement_name(finding.requirement);
}

} // namespace tessera::ui::style
