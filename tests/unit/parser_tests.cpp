// SPDX-License-Identifier: MIT

#include "tessera/ui/style/parser.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::style::parse_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "parser: " << message << "\n";
    }
    return condition;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    constexpr std::string_view source = R"(
/* comment */
.card:hover > .title {
  color: var(--text);
  padding: 8px 12px;
  opacity: 0.5 ! IMPORTANT;
  --gap: 4px;
}
)";

    auto sheet = parse_stylesheet(source);
    if (!expect(sheet.rules.size() == 1, "one rule") ||
        !expect(sheet.rules[0].selector == ".card:hover > .title", "selector prelude") ||
        !expect(sheet.rules[0].declarations.size() == 4, "four declarations") ||
        !expect(sheet.rules[0].offset == source.find(".card"), "rule offset")) {
        return 1;
    }

    const auto& declarations = sheet.rules[0].declarations;
    if (!expect(declarations[0].property == "color", "color property") ||
        !expect(declarations[0].value == "var(--text)", "function value") ||
        !expect(declarations[1].property == "padding", "padding property") ||
        !expect(declarations[1].value == "8px 12px", "multi-value") ||
        !expect(declarations[2].property == "opacity", "opacity property") ||
        !expect(declarations[2].value == "0.5", "important stripped") ||
        !expect(declarations[2].important, "important flag") ||
        !expect(declarations[3].property == "--gap", "custom property") ||
        !expect(declarations[3].value == "4px", "custom value")) {
        return 2;
    }

    constexpr std::string_view at_source = R"(
@import "tokens.css";
@media (min-width: 900px) { .a { color: red; } }
@keyframes pulse { from { opacity: 1; } to { opacity: 0.5; } }
.after { color: blue; }
)";
    sheet = parse_stylesheet(at_source);
    if (!expect(sheet.at_rules.size() == 3, "three at-rules") ||
        !expect(sheet.at_rules[0].name == "import", "import name") ||
        !expect(sheet.at_rules[0].prelude == "\"tokens.css\"", "import prelude") ||
        !expect(!sheet.at_rules[0].has_block, "import has no block") ||
        !expect(sheet.at_rules[1].name == "media", "media name") ||
        !expect(sheet.at_rules[1].prelude == "(min-width: 900px)", "media prelude") ||
        !expect(sheet.at_rules[1].has_block, "media block") ||
        !expect(contains(sheet.at_rules[1].block, ".a"), "media nested rule") ||
        !expect(contains(sheet.at_rules[1].block, "color: red"), "media nested declaration") ||
        !expect(sheet.at_rules[2].name == "keyframes", "keyframes name") ||
        !expect(contains(sheet.at_rules[2].block, "from"), "keyframes block") ||
        !expect(sheet.rules.size() == 1, "rule after at-rules")) {
        return 3;
    }

    constexpr std::string_view tolerant = ".bad { color red; width: 10px; } .good { color: blue; }";
    sheet = parse_stylesheet(tolerant);
    if (!expect(sheet.rules.size() == 2, "tolerant recovery keeps rules") ||
        !expect(sheet.rules[0].declarations.size() == 1, "missing colon skips one declaration") ||
        !expect(sheet.rules[0].declarations[0].property == "width", "recovered declaration") ||
        !expect(sheet.rules[1].declarations.size() == 1, "good rule declaration") ||
        !expect(sheet.rules[1].declarations[0].value == "blue", "good rule value")) {
        return 4;
    }

    sheet = parse_stylesheet("");
    if (!expect(sheet.rules.empty() && sheet.at_rules.empty(), "empty source")) {
        return 5;
    }

    sheet = parse_stylesheet("@media {");
    if (!expect(sheet.at_rules.size() == 1, "unterminated at-rule survives") ||
        !expect(sheet.at_rules[0].has_block, "unterminated at-rule has block")) {
        return 6;
    }

    sheet = parse_stylesheet(".a { color: red !important; }");
    if (!expect(sheet.rules.size() == 1, "compact important rule") ||
        !expect(sheet.rules[0].declarations[0].important, "compact important flag") ||
        !expect(sheet.rules[0].declarations[0].value == "red", "compact important value")) {
        return 7;
    }

    std::cout << "parser: ok\n";
    return 0;
}
