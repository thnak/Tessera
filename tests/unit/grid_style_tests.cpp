// SPDX-License-Identifier: MIT
//
// Registry and `@supports` coverage for the CSS Grid longhands (issue #249). The grammar is shared
// between the declaration validator, `@supports` and the layout engine (docs/specs/grid-v1.md), so
// these checks are the contract a stylesheet can rely on.

#include "tessera/ui/style/grid_values.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::Declaration;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_grid_line;
using tessera::ui::style::parse_grid_template;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "grid_style: " << message << "\n";
    }
    return condition;
}

bool matches(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

bool value_is(const PropertyRegistry& registry, const char* property, const char* value) {
    const auto found = registry.find(property);
    return found != nullptr && found->initial == value;
}

} // namespace

int main() {
    const PropertyRegistry registry;

    // 1. Every grid longhand is registered with its CSS initial value.
    if (!expect(registry.supported("grid-template-columns"), "grid-template-columns exists") ||
        !expect(registry.supported("grid-template-rows"), "grid-template-rows exists") ||
        !expect(registry.supported("grid-column"), "grid-column exists") ||
        !expect(registry.supported("grid-row"), "grid-row exists") ||
        !expect(registry.supported("grid-auto-flow"), "grid-auto-flow exists") ||
        !expect(registry.supported("justify-self"), "justify-self exists") ||
        !expect(registry.supported("justify-items"), "justify-items exists") ||
        !expect(registry.supported("align-items"), "align-items exists") ||
        !expect(registry.supported("align-self"), "align-self exists") ||
        !expect(registry.supported("row-gap"), "row-gap exists") ||
        !expect(registry.supported("column-gap"), "column-gap exists") ||
        !expect(value_is(registry, "grid-template-columns", "none"), "columns initial: none") ||
        !expect(value_is(registry, "grid-template-rows", "none"), "rows initial: none") ||
        !expect(value_is(registry, "grid-column", "auto"), "grid-column initial: auto") ||
        !expect(value_is(registry, "grid-row", "auto"), "grid-row initial: auto") ||
        !expect(value_is(registry, "grid-auto-flow", "row"), "grid-auto-flow initial: row") ||
        !expect(value_is(registry, "justify-self", "auto"), "justify-self initial: auto")) {
        return 1;
    }

    // 2. `gap` and `place-self` expand onto the grid longhands.
    const auto gap = registry.expand(Declaration{"gap", "10px 20px", false, 0});
    if (!expect(gap.diagnostics.empty(), "gap diagnostics") ||
        !expect(gap.declarations.size() == 2, "gap expands to two longhands") ||
        !expect(gap.declarations[0].property == "row-gap" && gap.declarations[0].value == "10px",
                "gap row") ||
        !expect(gap.declarations[1].property == "column-gap" && gap.declarations[1].value == "20px",
                "gap column")) {
        return 2;
    }
    const auto place_self = registry.expand(Declaration{"place-self", "center end", false, 0});
    if (!expect(place_self.diagnostics.empty(), "place-self diagnostics") ||
        !expect(place_self.declarations.size() == 2, "place-self expands") ||
        !expect(place_self.declarations[0].property == "align-self" &&
                    place_self.declarations[0].value == "center",
                "place-self align") ||
        !expect(place_self.declarations[1].property == "justify-self" &&
                    place_self.declarations[1].value == "end",
                "place-self justify")) {
        return 3;
    }

    // 3. The supported value grammar.
    if (!expect(parse_grid_template("none").valid, "none is a valid template") ||
        !expect(parse_grid_template("100px 1fr auto 25%").valid, "mixed track list") ||
        !expect(parse_grid_template("repeat(3, 1fr)").tracks.size() == 3, "repeat expands") ||
        !expect(parse_grid_template("repeat(2, 50px 1fr)").tracks.size() == 4,
                "repeat with two tracks") ||
        !expect(parse_grid_template("minmax(100px, 1fr)").valid, "minmax length/fr") ||
        !expect(parse_grid_template("min-content max-content").valid, "intrinsic keywords") ||
        !expect(!parse_grid_template("subgrid").valid, "subgrid is rejected") ||
        !expect(!parse_grid_template("masonry").valid, "masonry is rejected") ||
        !expect(!parse_grid_template("[header] 1fr").valid, "named lines are rejected") ||
        !expect(!parse_grid_template("repeat(auto-fill, 1fr)").valid,
                "auto-fill/auto-fit are rejected") ||
        !expect(!parse_grid_template("minmax(1fr, 2fr)").valid, "a flexible minimum is rejected") ||
        !expect(!parse_grid_template("\"a b\" 1fr").valid, "named areas are rejected")) {
        return 4;
    }

    if (!expect(parse_grid_line("auto").valid, "auto placement") ||
        !expect(parse_grid_line("2").valid && parse_grid_line("2").start == 2, "a line number") ||
        !expect(parse_grid_line("span 2").valid && parse_grid_line("span 2").span == 2, "span n") ||
        !expect(parse_grid_line("2 / 4").valid, "line / line") ||
        !expect(parse_grid_line("2 / span 3").span == 3, "line / span n") ||
        !expect(parse_grid_line("-1").start == -1, "negative line") ||
        !expect(!parse_grid_line("header").valid, "a named line is rejected") ||
        !expect(!parse_grid_line("span 2 / span 3").valid, "span on both sides is rejected")) {
        return 5;
    }

    // 4. `@supports` is exactly as strict as the declaration validator.
    if (!expect(matches("(display: grid)", registry), "supports display: grid") ||
        !expect(matches("(display: inline-grid)", registry), "supports display: inline-grid") ||
        !expect(matches("(grid-template-columns: repeat(3, 1fr))", registry), "supports repeat") ||
        !expect(matches("(grid-template-columns: minmax(100px, 1fr))", registry),
                "supports minmax") ||
        !expect(matches("(grid-template-rows: auto 1fr)", registry), "supports auto fr rows") ||
        !expect(matches("(grid-column: 2 / span 3)", registry), "supports grid-column") ||
        !expect(matches("(grid-row: span 2)", registry), "supports grid-row span") ||
        !expect(matches("(grid-auto-flow: column)", registry), "supports grid-auto-flow") ||
        !expect(matches("(justify-self: center)", registry), "supports justify-self") ||
        !expect(matches("(justify-items: stretch)", registry), "supports justify-items") ||
        !expect(!matches("(grid-template-columns: subgrid)", registry), "subgrid unsupported") ||
        !expect(!matches("(grid-template-columns: masonry)", registry), "masonry unsupported") ||
        !expect(!matches("(grid-auto-flow: row dense)", registry), "dense unsupported") ||
        !expect(!matches("(grid-column: header)", registry), "named line unsupported") ||
        !expect(!matches("(justify-self: safe center)", registry), "safe prefix unsupported")) {
        return 6;
    }

    // 5. The declaration validator drops an unsupported value and reports it, and keeps the
    //    supported forms (stylesheet-v1 section 11: unsupported declarations are never silent).
    const auto invalid_template =
        registry.expand(Declaration{"grid-template-columns", "subgrid", false, 7});
    if (!expect(invalid_template.declarations.empty(), "invalid template is dropped") ||
        !expect(invalid_template.diagnostics.size() == 1 &&
                    invalid_template.diagnostics[0].kind == StyleDiagnosticKind::UnsupportedValue &&
                    invalid_template.diagnostics[0].offset == 7,
                "invalid template diagnostic")) {
        return 7;
    }
    const auto invalid_flow = registry.expand(Declaration{"grid-auto-flow", "row dense", false, 0});
    if (!expect(invalid_flow.declarations.empty(), "dense is dropped") ||
        !expect(invalid_flow.diagnostics.size() == 1, "dense diagnostic")) {
        return 8;
    }
    const auto valid_template =
        registry.expand(Declaration{"grid-template-columns", "repeat(2, 1fr)", false, 0});
    if (!expect(valid_template.diagnostics.empty(), "valid template diagnostics") ||
        !expect(valid_template.declarations.size() == 1, "valid template is kept")) {
        return 9;
    }
    const auto valid_self = registry.expand(Declaration{"justify-self", "center", false, 0});
    if (!expect(valid_self.diagnostics.empty(), "valid justify-self diagnostics") ||
        !expect(valid_self.declarations.size() == 1, "valid justify-self is kept")) {
        return 10;
    }

    std::cout << "grid_style: ok\n";
    return 0;
}
