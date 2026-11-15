// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// WCAG 2.2 contrast for theme tokens (issue #79, ADR-047 item 5). This is the one contrast
// implementation in the repo: the generator solves against it, `tessera theme --check` lints a
// hand-edited token set with it, and the Instrument asset test asserts the shipped pairs with it
// (tests/unit/instrument_theme_tests.cpp calls `contrast_ratio`). Luminance is the WCAG relative
// luminance definition, which linearizes sRGB, not the OKLab L.
namespace tessera::ui::style {

struct ThemeAsset;

// Split of the WCAG thresholds by role, so "which threshold applies" is data, not a comment.
enum class ContrastRequirement : std::uint8_t {
    NormalText, // 4.5:1 (WCAG 2.2 SC 1.4.3); 7:1 in the high-contrast theme (AAA 1.4.6)
    LargeText,  // 3.0:1 (SC 1.4.3 large-text clause); 4.5:1 in high contrast
    NonText,    // 3.0:1 (SC 1.4.11 non-text contrast); 4.5:1 in high contrast
};

// The AA floor for a requirement, in the normal and the high-contrast variant. High contrast is
// defined as one step up the WCAG ladder: normal text 7:1, large text 4.5:1, non-text 4.5:1.
[[nodiscard]] double contrast_floor(ContrastRequirement requirement, bool high_contrast);

[[nodiscard]] constexpr const char* contrast_requirement_name(ContrastRequirement requirement) {
    switch (requirement) {
    case ContrastRequirement::NormalText:
        return "normal text";
    case ContrastRequirement::LargeText:
        return "large text";
    case ContrastRequirement::NonText:
        return "non-text";
    }
    return "normal text";
}

// WCAG 2.2 relative luminance of an opaque color: the sRGB channels linearized and weighted
// 0.2126/0.7152/0.0722. Alpha is ignored; token values are opaque (stylesheet-v1.md 9.2).
[[nodiscard]] double relative_luminance(const ColorRgba& color);

// (lighter + 0.05) / (darker + 0.05), the WCAG 2.2 contrast ratio. Symmetric, and 1.0 for a pair
// of identical colors.
[[nodiscard]] double contrast_ratio(const ColorRgba& a, const ColorRgba& b);

// A token pair a theme must satisfy, in the terms the checker reports. `foreground` and
// `background` are token names (with the leading `--`), so a diagnostic names the tokens a theme
// author edits rather than two hex values.
struct ContrastPair {
    std::string_view foreground;
    std::string_view background;
    ContrastRequirement requirement = ContrastRequirement::NormalText;
};

// The Instrument theme's contrast contract: every pair of the token set, with the threshold its
// role takes. This table is the single source for the generator's solve targets and for
// `check_theme_contrast`, so a role cannot be solved against one floor and linted against another.
[[nodiscard]] const std::vector<ContrastPair>& instrument_contrast_pairs();

struct ContrastFinding {
    std::string theme;  // theme id
    std::string pair;   // "--on-surface on --surface"
    double ratio = 0.0; // measured
    double required = 0.0;
    ContrastRequirement requirement = ContrastRequirement::NormalText;

    friend bool operator==(const ContrastFinding&, const ContrastFinding&) = default;
};

// Checks every pair of `instrument_contrast_pairs()` against each theme of `asset`, resolving the
// token values through the theme asset's own color parser (`parse_color`). A missing token, an
// unparseable value and an undershooting pair are all findings; an empty result means every pair
// of every theme passes.
//
// A theme that does not define `--surface` is checked only for the reserved chrome pair: the
// built-in `default` theme carries nothing else (stylesheet-v1.md 9.1/9.2), and a project theme may
// be chrome-only by design, so requiring the whole Instrument role table of it would report a false
// failure. A theme that does define `--surface` is a full token set and must define every role.
//
// `high_contrast_theme` is the id that gets the stricter floors (the caller knows its own naming;
// the generator and the CLI use a `-high-contrast` suffix).
[[nodiscard]] std::vector<ContrastFinding>
check_theme_contrast(const ThemeAsset& asset, std::string_view high_contrast_theme);

// One line per finding: `TESSERA_THEME_CONTRAST: theme `id`: `--on-surface` on `--surface` is
// 3.21:1, below the 4.5:1 floor for normal text`. The CLI prints these and exits 1.
[[nodiscard]] std::string describe_contrast_finding(const ContrastFinding& finding);

} // namespace tessera::ui::style
