// SPDX-License-Identifier: MIT

// The Button look (issue #239): `packages/tessera-ui/button.css` compiles with no diagnostics,
// every `var()` it reads is a token every Instrument theme defines (so no theme can leave a color
// or length unresolved), and every class the Button sets has a rule.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "library_button_style: " << message << "\n";
        ++failures;
    }
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::set<std::string> var_names(const std::string& css) {
    std::set<std::string> names;
    for (std::size_t at = css.find("var(--"); at != std::string::npos;
         at = css.find("var(--", at + 1)) {
        const std::size_t start = at + 4;
        const std::size_t end = css.find(')', start);
        names.insert(css.substr(start, end - start));
    }
    return names;
}

} // namespace

int main() {
    const std::string css = read_file(TESSERA_BUTTON_CSS);
    check(!css.empty(), "button.css is readable");

    const style::CompiledStyleSheet sheet = style::compile_stylesheet(css);
    check(sheet.diagnostics().empty(), "button.css compiles with no diagnostics");
    for (const style::StyleDiagnostic& diagnostic : sheet.diagnostics()) {
        std::cerr << "  " << diagnostic.property << ": " << diagnostic.message << "\n";
    }

    const auto theme_asset = style::parse_theme_asset(read_file(TESSERA_INSTRUMENT_THEME));
    check(theme_asset.has_value(), "the Instrument theme parses");
    if (!theme_asset) {
        return 1;
    }
    const std::set<std::string> names = var_names(css);
    check(names.size() >= 10, "button.css reads theme tokens");
    for (const char* id : {"instrument-light", "instrument-dark", "instrument-high-contrast"}) {
        const style::Theme* theme = theme_asset->find(id);
        check(theme != nullptr, std::string(id) + " exists");
        if (theme == nullptr) {
            continue;
        }
        for (const std::string& name : names) {
            check(theme->find(name) != nullptr, std::string(id) + " defines " + name);
        }
    }

    // The classes the Button sets (packages/tessera-ui/Button.tsx) each have a rule.
    for (const char* selector :
         {".tbtn ", ".tbtn-sm", ".tbtn-primary", ".tbtn-secondary", ".tbtn-ghost", ".tbtn-danger",
          ".tbtn-pressed", ".tbtn-disabled", ".tbtn:focus-visible", ".tbtn:hover"}) {
        check(css.find(selector) != std::string::npos, std::string("a rule for ") + selector);
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "library_button_style: ok\n";
    return 0;
}
