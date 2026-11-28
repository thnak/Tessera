// SPDX-License-Identifier: MIT

// The Icon look (issue #530): `packages/tessera-ui/icon.css` compiles with no diagnostics, names no
// color (the glyph takes the inherited `color`), gives both sizes a box, and flips only a
// directional icon: `ticon-mirror` scales by `--tessera-dir-sign` (1 by default, -1 where the
// computed `direction` is rtl, set by the engine), and `ticon-fixed` has no transform.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "library_icon_style: " << message << "\n";
        ++failures;
    }
}

std::string transform_of(const style::CompiledStyleSheet& sheet, const char* second_class,
                         const style::ComputedStyle* parent) {
    style::StyleNode node;
    node.tag = "View";
    node.classes = {"ticon", "ticon-md", second_class};
    const style::ComputedStyle computed = sheet.compute(node, parent);
    const std::string* value = computed.find("transform");
    return value == nullptr ? "<none>" : *value;
}

std::string width_of(const style::CompiledStyleSheet& sheet, const char* size_class) {
    style::StyleNode node;
    node.tag = "View";
    node.classes = {"ticon", size_class};
    const style::ComputedStyle computed = sheet.compute(node, nullptr);
    const std::string* value = computed.find("width");
    return value == nullptr ? "<none>" : *value;
}

} // namespace

int main() {
    std::ifstream in(TESSERA_ICON_CSS, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const std::string css = text.str();
    check(!css.empty(), "icon.css is readable");
    check(css.find("color:") == std::string::npos, "icon.css names no color");

    const style::CompiledStyleSheet sheet = style::compile_stylesheet(css);
    check(sheet.diagnostics().empty(), "icon.css compiles with no diagnostics");
    for (const style::StyleDiagnostic& diagnostic : sheet.diagnostics()) {
        std::cerr << "  " << diagnostic.property << ": " << diagnostic.message << "\n";
    }

    check(width_of(sheet, "ticon-sm") == "16px", "sm is 16px: " + width_of(sheet, "ticon-sm"));
    check(width_of(sheet, "ticon-md") == "20px", "md is 20px: " + width_of(sheet, "ticon-md"));

    style::ComputedStyle rtl_root;
    rtl_root.set("direction", "rtl");
    // An ltr island inside an rtl root: the parent still carries the inherited signal.
    style::ComputedStyle ltr_island;
    ltr_island.set("direction", "ltr");
    ltr_island.set("--tessera-dir-sign", "-1");
    check(transform_of(sheet, "ticon-mirror", &ltr_island) == "scaleX(1)",
          "an ltr island inside an rtl root does not flip: " +
              transform_of(sheet, "ticon-mirror", &ltr_island));
    check(transform_of(sheet, "ticon-mirror", nullptr) == "scaleX(1)",
          "a directional icon is not flipped by default: " +
              transform_of(sheet, "ticon-mirror", nullptr));
    check(transform_of(sheet, "ticon-mirror", &rtl_root) == "scaleX(-1)",
          "a directional icon flips where direction is rtl: " +
              transform_of(sheet, "ticon-mirror", &rtl_root));
    check(transform_of(sheet, "ticon-fixed", &rtl_root) == "none" ||
              transform_of(sheet, "ticon-fixed", &rtl_root) == "<none>",
          "a non-directional icon never flips: " + transform_of(sheet, "ticon-fixed", &rtl_root));

    if (failures != 0) {
        return 1;
    }
    std::cout << "library_icon_style: ok\n";
    return 0;
}
