// SPDX-License-Identifier: MIT

// The Instrument theme asset (ADR-047 items 4 and 5, issue #237): `packages/tessera-ui/themes/
// instrument.theme.json` parses cleanly, defines the same tokens in light, dark and high-contrast,
// carries its theme version, and meets WCAG 2.2 AA contrast for every text/background and
// indicator/background token pair of each theme (high-contrast text meets 7:1).

#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "instrument_theme: " << message << "\n";
        ++failures;
    }
}

double channel(float value) {
    const double v = value;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double luminance(const style::ColorRgba& c) {
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

double contrast(const style::ColorRgba& a, const style::ColorRgba& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool color_of(const style::Theme& theme, std::string_view token, style::ColorRgba& out) {
    const style::ThemeToken* found = theme.find(token);
    if (found == nullptr) {
        return false;
    }
    const style::ColorParseResult parsed = style::parse_color(found->value);
    out = parsed.color;
    return parsed.ok;
}

using Pairs = std::vector<std::pair<std::string_view, std::string_view>>;

// Text on its background: normal text needs 4.5:1 (WCAG 1.4.3), 7:1 in the high-contrast theme.
const Pairs kTextPairs{
    {"--on-surface", "--surface"},
    {"--on-surface", "--surface-raised"},
    {"--on-surface", "--surface-sunken"},
    {"--on-surface-muted", "--surface"},
    {"--on-surface-muted", "--surface-raised"},
    {"--on-surface-muted", "--surface-sunken"},
    {"--on-accent", "--accent"},
    {"--on-danger", "--danger"},
    {"--on-success", "--success"},
    {"--on-warning", "--warning"},
};

// Fills and indicators against the surface they sit on need 3:1 (WCAG 1.4.11).
const Pairs kIndicatorPairs{
    {"--accent", "--surface"},
    {"--focus-ring", "--surface"},
    {"--focus-ring", "--surface-raised"},
    {"--border-strong", "--surface"},
    {"--danger", "--surface"},
    {"--success", "--surface"},
    {"--warning", "--surface"},
};

void check_pairs(const style::Theme& theme, const Pairs& pairs, double minimum,
                 std::string_view what) {
    for (const auto& [foreground, background] : pairs) {
        style::ColorRgba fg;
        style::ColorRgba bg;
        const std::string label =
            theme.id + ": " + std::string(foreground) + " on " + std::string(background);
        if (!color_of(theme, foreground, fg) || !color_of(theme, background, bg)) {
            check(false, label + " must be two parseable colors");
            continue;
        }
        const double ratio = contrast(fg, bg);
        check(ratio >= minimum, label + " is " + std::to_string(ratio) + ":1, below " +
                                    std::to_string(minimum) + ":1 for " + std::string(what));
    }
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

int pixels_of(const style::Theme* theme, std::string_view name) {
    const style::ThemeToken* token = theme != nullptr ? theme->find(name) : nullptr;
    return token != nullptr ? std::atoi(token->value.c_str()) : -1;
}

} // namespace

int main() {
    const std::string text = read_file(TESSERA_INSTRUMENT_THEME);
    check(!text.empty(), "the theme file is readable");
    const auto parsed = style::parse_theme_asset(text);
    check(parsed.has_value(), "the theme file parses");
    if (!parsed) {
        return 1;
    }
    check(parsed->warnings.empty(),
          "every theme defines the same tokens (no TESSERA_THEME_TOKEN_MISSING)");
    check(parsed->start == "instrument-light", "the start theme is instrument-light");

    const std::vector<std::string> ids{"instrument-light", "instrument-dark",
                                       "instrument-high-contrast"};
    const style::Theme* light = parsed->find("instrument-light");
    for (const std::string& id : ids) {
        const style::Theme* theme = parsed->find(id);
        check(theme != nullptr, id + " exists");
        if (theme == nullptr) {
            continue;
        }
        check(theme->version == "1.0.0", id + " is version 1.0.0");
        check(style::theme_version_satisfies_pin(theme->version, "1"), id + " satisfies pin 1");
        const bool high_contrast = id == "instrument-high-contrast";
        check_pairs(*theme, kTextPairs, high_contrast ? 7.0 : 4.5,
                    high_contrast ? "high-contrast text" : "AA text");
        check_pairs(*theme, kIndicatorPairs, 3.0, "AA indicators");
        check(theme->find("--window-background") != nullptr &&
                  theme->find("--window-foreground") != nullptr,
              id + " defines the reserved chrome pair");

        // The themes carry an identical token name set in the same order.
        if (light != nullptr && theme->tokens.size() == light->tokens.size()) {
            for (std::size_t i = 0; i < theme->tokens.size(); ++i) {
                check(theme->tokens[i].name == light->tokens[i].name,
                      id + " token order matches at " + light->tokens[i].name);
            }
        } else {
            check(false, id + " has the same token count as instrument-light");
        }

        // Spacing stays on the 4 px grid and radii in the 2 to 8 px range (ADR-047 item 5).
        for (const std::string_view space :
             {"--space-1", "--space-2", "--space-3", "--space-4", "--space-6", "--space-8"}) {
            const int px = pixels_of(theme, space);
            check(px > 0 && px % 4 == 0, id + " " + std::string(space) + " is on the 4 px grid");
        }
        for (const std::string_view radius : {"--radius-sm", "--radius-md", "--radius-lg"}) {
            const int px = pixels_of(theme, radius);
            check(px >= 2 && px <= 8, id + " " + std::string(radius) + " is 2 to 8 px");
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "instrument_theme: ok\n";
    return 0;
}
