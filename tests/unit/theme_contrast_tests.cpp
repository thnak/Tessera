// SPDX-License-Identifier: MIT

// The WCAG 2.2 contrast checker (issue #79 task 3): relative luminance and the ratio against the
// WCAG reference values, the floor each contrast role takes, the Instrument pair table, and the
// checker's own behaviour on a token set that is deliberately broken.

#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"

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
        std::cerr << "theme_contrast: " << message << "\n";
        ++failures;
    }
}

void check_near(double actual, double expected, double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "theme_contrast: " << message << ": expected " << expected << ", got "
                  << actual << "\n";
        ++failures;
    }
}

style::ColorRgba hex(std::string_view text) {
    return style::parse_color(text).color;
}

// A one-theme asset whose tokens are exactly `tokens`, so the checker can be pointed at a pair.
style::ThemeAsset asset_with(std::string_view id, std::vector<style::ThemeToken> tokens) {
    style::ThemeAsset asset;
    asset.start = std::string(id);
    asset.themes.push_back(style::Theme{std::string(id), {}, std::move(tokens)});
    return asset;
}

// A complete Instrument light token set that passes every floor, used as the baseline for the
// deliberately broken variants below.
std::vector<style::ThemeToken> passing_tokens() {
    return {{"--surface", "#ffffff"},          {"--surface-raised", "#f1f6fd"},
            {"--surface-sunken", "#e7ecf2"},   {"--on-surface", "#171b1f"},
            {"--on-surface-muted", "#54585e"}, {"--on-accent", "#ffffff"},
            {"--accent", "#3064a2"},           {"--on-danger", "#ffffff"},
            {"--danger", "#ac3031"},           {"--on-success", "#ffffff"},
            {"--success", "#1d7635"},          {"--on-warning", "#ffffff"},
            {"--warning", "#7e5e00"},          {"--focus-ring", "#1663b4"},
            {"--border-strong", "#6e7277"},    {"--window-background", "#ffffff"},
            {"--window-foreground", "#171b1f"}};
}

} // namespace

int main() {
    // WCAG relative luminance of the extremes, and the sRGB primaries' published values.
    check_near(style::relative_luminance(hex("#000000")), 0.0, 1e-9, "black luminance");
    check_near(style::relative_luminance(hex("#ffffff")), 1.0, 1e-9, "white luminance");
    check_near(style::relative_luminance(hex("#ff0000")), 0.2126, 1e-4, "red luminance");
    check_near(style::relative_luminance(hex("#00ff00")), 0.7152, 1e-4, "green luminance");
    check_near(style::relative_luminance(hex("#0000ff")), 0.0722, 1e-4, "blue luminance");

    // The ratio: 21:1 for black on white, 1:1 for a color on itself, and symmetric.
    check_near(style::contrast_ratio(hex("#000000"), hex("#ffffff")), 21.0, 1e-9, "black on white");
    check_near(style::contrast_ratio(hex("#0b5cad"), hex("#0b5cad")), 1.0, 1e-12, "self ratio");
    check_near(style::contrast_ratio(hex("#0b5cad"), hex("#ffffff")),
               style::contrast_ratio(hex("#ffffff"), hex("#0b5cad")), 1e-12, "symmetric");
    // #767676 is the canonical "just passes 4.5:1 on white" grey, #777777 the canonical miss.
    check(style::contrast_ratio(hex("#767676"), hex("#ffffff")) >= 4.5, "#767676 clears 4.5:1");
    check(style::contrast_ratio(hex("#777777"), hex("#ffffff")) < 4.5, "#777777 misses 4.5:1");
    // Alpha is ignored: token values are opaque (stylesheet-v1.md 9.2).
    check_near(style::contrast_ratio(hex("rgba(0, 0, 0, 0.5)"), hex("#ffffff")), 21.0, 1e-9,
               "alpha does not affect the ratio");

    // The floors per role, normal and high contrast.
    check_near(style::contrast_floor(style::ContrastRequirement::NormalText, false), 4.5, 0.0,
               "normal text AA");
    check_near(style::contrast_floor(style::ContrastRequirement::NormalText, true), 7.0, 0.0,
               "normal text high contrast");
    check_near(style::contrast_floor(style::ContrastRequirement::LargeText, false), 3.0, 0.0,
               "large text AA");
    check_near(style::contrast_floor(style::ContrastRequirement::LargeText, true), 4.5, 0.0,
               "large text high contrast");
    check_near(style::contrast_floor(style::ContrastRequirement::NonText, false), 3.0, 0.0,
               "non-text AA");
    check_near(style::contrast_floor(style::ContrastRequirement::NonText, true), 4.5, 0.0,
               "non-text high contrast");

    // The pair table names every role the generated token set carries, and names no token twice as
    // a foreground at the same requirement (a duplicate would mean two conflicting floors).
    const std::vector<style::ContrastPair>& pairs = style::instrument_contrast_pairs();
    check(!pairs.empty(), "the pair table is not empty");
    for (const style::ContrastPair& pair : pairs) {
        check(pair.foreground.starts_with("--"), std::string(pair.foreground) + " is a token");
        check(pair.background.starts_with("--"), std::string(pair.background) + " is a token");
        check(pair.foreground != pair.background, "a pair never names one token twice");
    }
    // Every role the Instrument token set carries is checked, against the background it actually
    // sits on (a fill's ink sits on the fill, page text on the three surfaces).
    const auto has_pair = [&pairs](std::string_view foreground, std::string_view background) {
        for (const style::ContrastPair& pair : pairs) {
            if (pair.foreground == foreground && pair.background == background) {
                return true;
            }
        }
        return false;
    };
    for (const std::string_view surface : {"--surface", "--surface-raised", "--surface-sunken"}) {
        for (const std::string_view foreground : {"--on-surface", "--on-surface-muted"}) {
            check(has_pair(foreground, surface),
                  std::string(foreground) + " on " + std::string(surface) + " is checked");
        }
        for (const std::string_view indicator : {"--accent", "--focus-ring", "--border-strong",
                                                 "--danger", "--success", "--warning"}) {
            check(has_pair(indicator, surface), std::string(indicator) + " on " +
                                                    std::string(surface) +
                                                    " is checked as an indicator or large text");
        }
    }
    for (const std::string_view fill : {"--accent", "--danger", "--success", "--warning"}) {
        const std::string ink = "--on" + std::string(fill.substr(1));
        check(has_pair(ink, fill), ink + " on " + std::string(fill) + " is checked as text");
    }
    check(has_pair("--window-foreground", "--window-background"),
          "the reserved chrome pair is checked");

    // The accent is large text, not normal text: ADR-047 item 5 forbids the accent as the only
    // carrier of small text, so the table must not grant it the 4.5:1 text floor as a fill.
    for (const style::ContrastPair& pair : pairs) {
        if (pair.foreground == "--accent" && pair.background.starts_with("--surface")) {
            check(pair.requirement == style::ContrastRequirement::LargeText,
                  "--accent on " + std::string(pair.background) + " is large text");
        }
    }

    // One deliberately bad pair on an otherwise complete set is reported, and only that pair.
    auto broken = asset_with("instrument-light", passing_tokens());
    for (style::ThemeToken& token : broken.themes.front().tokens) {
        if (token.name == "--on-surface") {
            token.value = "#f0f0f0";
        }
    }
    const std::vector<style::ContrastFinding> broken_findings =
        style::check_theme_contrast(broken, "instrument-high-contrast");
    check(broken_findings.size() == 3, "three on-surface pairs fail on a near-white ink (got " +
                                           std::to_string(broken_findings.size()) + ")");
    for (const style::ContrastFinding& finding : broken_findings) {
        check(finding.theme == "instrument-light", "the finding names the theme");
        check(finding.pair.starts_with("--on-surface on --surface"),
              "the finding names the tokens (" + finding.pair + ")");
        check(finding.requirement == style::ContrastRequirement::NormalText,
              "on-surface is normal text");
        check(finding.ratio < finding.required, "a finding reports a real miss");
        const std::string line = style::describe_contrast_finding(finding);
        check(line.starts_with("TESSERA_THEME_CONTRAST: "), "the diagnostic code leads");
        check(line.find("--on-surface") != std::string::npos, "the diagnostic names the token");
        check(line.find("4.5:1 floor for normal text") != std::string::npos,
              "the diagnostic names the floor and the role");
    }

    // An unparseable or missing token is a finding, not a crash.
    auto missing = asset_with("instrument-light", {{"--surface", "#ffffff"}});
    const std::vector<style::ContrastFinding> missing_findings =
        style::check_theme_contrast(missing, "instrument-high-contrast");
    check(!missing_findings.empty(), "a missing token is reported");
    check(missing_findings.front().pair.find("missing or unparseable") != std::string::npos,
          "the finding says the token is missing");

    // A complete, passing one-theme set produces no findings at the normal floors.
    auto passing = asset_with("instrument-light", passing_tokens());
    check(style::check_theme_contrast(passing, "instrument-high-contrast").empty(),
          "a passing token set produces no findings");
    // The same set under the high-contrast id fails, because that is what the id opts into: the
    // high-contrast floors are 7:1 text and 4.5:1 non-text, and this set was built for AA.
    passing.themes.front().id = "instrument-high-contrast";
    check(!style::check_theme_contrast(passing, "instrument-high-contrast").empty(),
          "the high-contrast floors are stricter");
    // And a token set with no high-contrast theme id never gets them, even when one is named.
    auto normal = asset_with("instrument-light", passing_tokens());
    check(style::check_theme_contrast(normal, "instrument-high-contrast").empty(),
          "a light-only asset takes the normal floors");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "theme_contrast: ok\n";
    return 0;
}
