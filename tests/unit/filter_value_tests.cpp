// SPDX-License-Identifier: MIT
//
// The `filter` grammar (issue #617): `none | [blur(<length>) | drop-shadow(<color>?
// <length>{2,3})]+`. Adjacent blurs fold exactly, no-op functions are dropped, any other function
// invalidates the whole declaration, and the registry reports it once while `@supports` agrees with
// it.

#include "tessera/ui/style/filter_value.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace tessera::ui::style;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "filter_value: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 1.0e-4f;
}

bool blur_forms() {
    bool ok = true;
    const FilterParseResult px = parse_filter(" blur(4px) ");
    ok = expect(px.ok() && px.filter.count == 1 &&
                    px.filter.functions[0].kind == FilterFunction::Kind::Blur &&
                    near(px.filter.functions[0].sigma, 4.0f),
                "blur(4px)") &&
         ok;
    ok = expect(parse_filter("BLUR(2.5)").filter.functions[0].sigma == 2.5f,
                "case-insensitive, bare number") &&
         ok;
    const FilterParseResult folded = parse_filter("blur(3px) blur(4px)");
    ok = expect(folded.filter.count == 1 && near(folded.filter.functions[0].sigma, 5.0f),
                "adjacent blurs fold: sqrt(3^2 + 4^2)") &&
         ok;
    return ok;
}

bool drop_shadow_forms() {
    bool ok = true;
    const FilterParseResult full = parse_filter("drop-shadow(2px -3px 4px #ff000080)");
    const FilterFunction& f = full.filter.functions[0];
    ok = expect(full.ok() && f.kind == FilterFunction::Kind::DropShadow && f.offset_x == 2.0f &&
                    f.offset_y == -3.0f && f.sigma == 4.0f && !f.current_color &&
                    near(f.color.r, 1.0f) && near(f.color.a, 128.0f / 255.0f),
                "offsets, sigma and colour") &&
         ok;
    const FilterParseResult leading = parse_filter("drop-shadow(rgba(0, 0, 0, 0.4) 1px 2px)");
    ok = expect(leading.ok() && leading.filter.functions[0].sigma == 0.0f &&
                    near(leading.filter.functions[0].color.a, 0.4f),
                "colour first, with commas inside rgba(), and a zero blur") &&
         ok;
    ok = expect(parse_filter("drop-shadow(1px 1px)").filter.functions[0].current_color,
                "an omitted colour is currentcolor") &&
         ok;
    ok = expect(parse_filter("drop-shadow(1px 1px currentColor)").filter.functions[0].current_color,
                "so is the keyword") &&
         ok;
    return ok;
}

bool lists_and_noops() {
    bool ok = true;
    const FilterParseResult chain = parse_filter("blur(1px) drop-shadow(1px 1px red) blur(2px)");
    ok = expect(chain.ok() && chain.filter.count == 3, "three functions keep their order") && ok;
    ok = expect(parse_filter("none").ok() && parse_filter("none").filter.none(), "none") && ok;
    ok = expect(parse_filter("blur(0)").ok() && parse_filter("blur(0)").filter.none(),
                "blur(0) is dropped") &&
         ok;
    ok = expect(parse_filter("drop-shadow(5px 5px transparent)").filter.none(),
                "an invisible unblurred shadow is dropped") &&
         ok;
    ok = expect(parse_filter("blur(0) blur(2px) blur(0)").filter.count == 1,
                "no-ops vanish mid-list") &&
         ok;
    return ok;
}

bool rejects() {
    bool ok = true;
    for (const char* value :
         {"",
          "blur()",
          "blur(-1px)",
          "blur(2em)",
          "blur(50%)",
          "brightness(-1)",
          "sepia(1px)",
          "url(#f)",
          "hue-rotate(10)",
          "grayscale(1 2)",
          "opacity(none)",
          "blur",
          "blur(2px",
          "drop-shadow()",
          "drop-shadow(1px)",
          "drop-shadow(1px 1px 1px 1px)",
          "drop-shadow(1px 1px -1px red)",
          "drop-shadow(1px 1px red blue)",
          "drop-shadow(1px 1px notacolour)",
          "blur(1px) , blur(2px)",
          "blur(1px) drop-shadow(1px 1px 1px red) blur(1px) drop-shadow(1px 1px 1px red)"}) {
        const FilterParseResult parsed = parse_filter(value);
        ok = expect(!parsed.ok() && !parsed.error.empty(), std::string("rejected: ") + value) && ok;
    }
    const FilterParseResult unknown = parse_filter("blur(1px) drop-shadow(1px 1px) url(#f)");
    return expect(unknown.token == "url", "the diagnostic names the function") && ok;
}

bool registry_and_supports() {
    PropertyRegistry registry;
    Declaration valid;
    valid.property = "filter";
    valid.value = "blur(4px)";
    bool ok = expect(registry.expand(valid).diagnostics.empty(), "a valid filter is accepted");
    Declaration invalid = valid;
    invalid.value = "blur(4px) url(#f)";
    const PropertyExpansionResult result = registry.expand(invalid);
    ok = expect(result.declarations.empty() && result.diagnostics.size() == 1 &&
                    result.diagnostics[0].kind == StyleDiagnosticKind::UnsupportedValue &&
                    result.diagnostics[0].message.find("url") != std::string::npos,
                "an unsupported function drops the whole declaration with one diagnostic") &&
         ok;
    const auto supported = [&registry](const char* condition) {
        const SupportsParseResult parsed = parse_supports_condition(condition);
        return parsed.ok && evaluate_supports(parsed.condition, registry);
    };
    ok = expect(supported("(filter: drop-shadow(1px 1px 2px black))") &&
                    supported("(filter: sepia(1))") && !supported("(filter: sepia(-1))"),
                "@supports is as strict as the validator") &&
         ok;
    return ok;
}

} // namespace

int main() {
    const bool ok = blur_forms() && drop_shadow_forms() && lists_and_noops() && rejects() &&
                    registry_and_supports();
    if (!ok) {
        return 1;
    }
    std::cout << "filter_value: ok\n";
    return 0;
}
