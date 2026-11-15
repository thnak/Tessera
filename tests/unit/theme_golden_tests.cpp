// SPDX-License-Identifier: MIT

// Golden bytes for generated token sets (issue #79). Two seeds, chosen for the reasons the issue
// gives: a saturated orange (#ff6a00), which has the largest in-gamut chroma and the smallest
// headroom above the text floor, and pure blue (#0000ff), which has the deepest base hue. The
// golden is the exact text `format_theme_asset` produces, so a change to the derivation, the token
// order, the number formatting or the JSON layout shows up as a byte diff here.

#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace style = tessera::ui::style;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "theme_golden: " << message << "\n";
        ++failures;
    }
}

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// A one-line, position-named mismatch report: the first differing byte and the line it starts.
std::string first_difference(std::string_view actual, std::string_view expected) {
    const std::size_t limit = std::min(actual.size(), expected.size());
    std::size_t offset = 0;
    while (offset < limit && actual[offset] == expected[offset]) {
        ++offset;
    }
    std::size_t line = 1;
    for (std::size_t i = 0; i < offset && i < actual.size(); ++i) {
        line += actual[i] == '\n' ? 1u : 0u;
    }
    return "first difference at byte " + std::to_string(offset) + " (line " + std::to_string(line) +
           "), actual " + std::to_string(actual.size()) + " bytes vs golden " +
           std::to_string(expected.size()) + " bytes";
}

void check_golden(std::string_view seed, std::string_view file) {
    const auto asset = style::generate_instrument_theme(seed);
    check(asset.has_value(), "seed " + std::string(seed) + " generates");
    if (!asset) {
        return;
    }
    const std::string actual = style::format_theme_asset(*asset);
    const fs::path golden = fs::path(TESSERA_THEME_GOLDEN_DIR) / file;
    const std::string expected = read_text(golden);
    check(!expected.empty(), "the golden " + golden.string() + " is readable");
    if (actual != expected) {
        check(false, "seed " + std::string(seed) + " differs from " + golden.string() + ": " +
                         first_difference(actual, expected));
    }
    // A golden is only useful if it is a valid token set too.
    const auto parsed = style::parse_theme_asset(actual);
    check(parsed.has_value(), "seed " + std::string(seed) + " formats a valid asset");
    if (parsed) {
        check(parsed->warnings.empty(), "the golden asset has no token-missing warning");
        check(style::check_theme_contrast(*parsed, "instrument-high-contrast").empty(),
              "the golden asset clears every contrast floor");
    }
}

} // namespace

int main() {
    check_golden("#ff6a00", "orange.theme.json");
    check_golden("#0000ff", "blue.theme.json");

    // Every generated file ends with exactly one newline and uses two-space indentation, so a diff
    // is readable and a text editor cannot add trailing whitespace that the next run would drop.
    const auto asset = style::generate_instrument_theme("#ff6a00");
    if (asset) {
        const std::string text = style::format_theme_asset(*asset);
        check(!text.empty() && text.back() == '\n', "the output ends with a newline");
        check(text.find("\r") == std::string::npos, "the output has no carriage return");
        check(text.find("\n      \"--") != std::string::npos,
              "tokens are indented six spaces, one per line");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "theme_golden: ok\n";
    return 0;
}
