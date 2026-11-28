// SPDX-License-Identifier: MIT

// The Switch, Checkbox and Radio looks (issue #240): each stylesheet compiles with no diagnostics,
// reads only tokens every Instrument theme defines, names no color literal (so every theme and the
// high-contrast theme restyle it), and has a rule for every class its component sets.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "library_controls_style: " << message << "\n";
        ++failures;
    }
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The header comments cite issue numbers ("#240"), which are not colors.
std::string strip_comments(std::string css) {
    for (std::size_t at = css.find("/*"); at != std::string::npos; at = css.find("/*")) {
        const std::size_t end = css.find("*/", at + 2);
        css.erase(at, end == std::string::npos ? std::string::npos : end + 2 - at);
    }
    return css;
}

std::set<std::string> var_names(const std::string& css) {
    std::set<std::string> names;
    for (std::size_t at = css.find("var(--"); at != std::string::npos;
         at = css.find("var(--", at + 1)) {
        const std::size_t start = at + 4;
        names.insert(css.substr(start, css.find(')', start) - start));
    }
    return names;
}

struct Sheet {
    const char* file;
    std::vector<const char*> selectors;
    std::size_t min_tokens = 8; // a small look (a track and a fill) reads fewer tokens
};

} // namespace

int main() {
    const auto theme_asset = style::parse_theme_asset(read_file(TESSERA_INSTRUMENT_THEME));
    check(theme_asset.has_value(), "the Instrument theme parses");
    if (!theme_asset) {
        return 1;
    }
    const std::vector<Sheet> sheets{
        {"switch.css",
         {".tsw ", ".tsw-track", ".tsw-thumb", ".tsw-on .tsw-track", ".tsw-on .tsw-thumb",
          ".tsw-label", ".tsw:focus-visible", ".tsw:hover", ".tsw-disabled"}},
        {"checkbox.css",
         {".tchk ", ".tchk-box", ".tchk-on .tchk-box", ".tchk-mixed .tchk-box", ".tchk-bar",
          ".tchk-label", ".tchk:focus-visible", ".tchk:hover", ".tchk-disabled"}},
        {"radio.css",
         {".trg ", ".trad ", ".trad-dot", ".trad-on .trad-dot", ".trad-mark", ".trad-label",
          ".trad:focus-visible", ".trad:hover", ".trad-disabled"}},
        {"range.css",
         {".trng ", ".trng-track", ".trng-fill", ".trng-rest", ".trng-thumb", ".trng:focus-visible",
          ".trng:hover"},
         6},
        {"progress.css",
         {".tprg-track", ".tprg-fill", ".tprg-rest", ".tmtr-track", ".tmtr-fill", ".tmtr-good",
          ".tmtr-warn", ".tmtr-bad"},
         4},
        {"disclosure.css",
         {".tdet ", ".tdet-summary", ".tdet-open .tdet-summary", ".tdet-body",
          ".tdet-summary:focus-visible"}},
    };
    for (const Sheet& entry : sheets) {
        const std::string name = entry.file;
        const std::string css = read_file(std::string(TESSERA_LIBRARY_DIR) + "/" + name);
        check(!css.empty(), name + " is readable");
        const style::CompiledStyleSheet sheet = style::compile_stylesheet(css);
        check(sheet.diagnostics().empty(), name + " compiles with no diagnostics");
        for (const style::StyleDiagnostic& diagnostic : sheet.diagnostics()) {
            std::cerr << "  " << diagnostic.property << ": " << diagnostic.message << "\n";
        }
        const std::string rules = strip_comments(css);
        check(rules.find("rgb(") == std::string::npos && rules.find('#') == std::string::npos,
              name + " names no color literal");
        const std::set<std::string> names = var_names(css);
        check(names.size() >= entry.min_tokens, name + " reads theme tokens");
        for (const char* id : {"instrument-light", "instrument-dark", "instrument-high-contrast"}) {
            const style::Theme* theme = theme_asset->find(id);
            check(theme != nullptr, std::string(id) + " exists");
            for (const std::string& token : names) {
                check(theme != nullptr && theme->find(token) != nullptr,
                      std::string(id) + " defines " + token + " (" + name + ")");
            }
        }
        for (const char* selector : entry.selectors) {
            check(css.find(selector) != std::string::npos, name + ": a rule for " + selector);
        }
    }
    if (failures != 0) {
        return 1;
    }
    std::cout << "library_controls_style: ok\n";
    return 0;
}
