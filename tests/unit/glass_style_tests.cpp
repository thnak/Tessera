// SPDX-License-Identifier: MIT
//
// Style surface of glass (issue #244, docs/specs/glass-v1.md sections 2 and 3): the
// `backdrop-filter` / `-webkit-backdrop-filter` grammar, the UnsupportedValue diagnostic that
// ignores the whole declaration, the initial value, the alias rewrite and `@supports`.

#include "tessera/ui/style/backdrop_filter.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_backdrop_filter;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "glass_style: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-5f) {
    return std::abs(value - expected) <= tolerance;
}

bool parses(std::string_view value) {
    return parse_backdrop_filter(value).ok();
}

bool names_a_token(std::string_view value) {
    const auto parsed = parse_backdrop_filter(value);
    return !parsed.ok() && !parsed.token.empty();
}

bool supports(std::string_view text) {
    const PropertyRegistry registry;
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

// --- grammar ---------------------------------------------------------------------------------

bool grammar() {
    bool ok = true;
    const auto none = parse_backdrop_filter("none");
    ok = expect(none.ok() && none.filter.none(), "`none` is a pass-through") && ok;

    const auto blur = parse_backdrop_filter("blur(8px)");
    ok = expect(blur.ok() && near(blur.filter.blur_sigma, 8.0f), "blur(px) parses") && ok;

    const auto zero = parse_backdrop_filter("blur(0)");
    ok = expect(zero.ok() && zero.filter.none(), "blur(0) is a pass-through") && ok;

    const auto spaced = parse_backdrop_filter("  blur( 8.5px )  ");
    ok = expect(spaced.ok() && near(spaced.filter.blur_sigma, 8.5f),
                "surrounding whitespace and px decimals parse") &&
         ok;

    // Two Gaussians convolve to sqrt(a^2 + b^2): the written order is preserved exactly.
    const auto twice = parse_backdrop_filter("blur(4px) blur(3px)");
    ok = expect(twice.ok() && near(twice.filter.blur_sigma, 5.0f),
                "repeated blur folds as sqrt(a^2 + b^2)") &&
         ok;

    const auto listed =
        parse_backdrop_filter("blur(20px) saturate(1.4) brightness(80%) contrast(2)");
    ok = expect(listed.ok() && near(listed.filter.blur_sigma, 20.0f) &&
                    near(listed.filter.saturate, 1.4f) && near(listed.filter.brightness, 0.8f) &&
                    near(listed.filter.contrast, 2.0f),
                "a written-order list parses all four functions") &&
         ok;

    const auto percent = parse_backdrop_filter("saturate(140%)");
    ok = expect(percent.ok() && near(percent.filter.saturate, 1.4f), "percent factors fold") && ok;

    const auto repeated = parse_backdrop_filter("saturate(2) saturate(3)");
    ok = expect(repeated.ok() && near(repeated.filter.saturate, 6.0f),
                "repeated color operators multiply") &&
         ok;

    const auto upper = parse_backdrop_filter("BLUR(2PX) SATURATE(2)");
    ok = expect(upper.ok() && near(upper.filter.blur_sigma, 2.0f),
                "function names are case-insensitive") &&
         ok;

    return ok;
}

bool grammar_rejects() {
    bool ok = true;
    const char* const invalid[] = {
        "",
        "   ",
        "none blur(1px)", // `none` must stand alone
        "blur(10%)",      // percentages and negative lengths are invalid
        "blur(-2px)",
        "blur(2em)",
        "blur()",
        "blur(",
        "blur(1px",
        "sepia(1)", // any other function is UnsupportedValue
        "hue-rotate(90deg)",
        "blur(1px) sepia(0.5)", // one unsupported function ignores the whole declaration
        "drop-shadow(0 0 2px black)",
        "blur(1px) 4px", // a bare token is not a function
        "blur(nan)",
        "blur(inf)",
        "blur(1e999)", // not a finite CSS number
        "saturate(-1)",
        "contrast(2deg)",
    };
    for (const char* value : invalid) {
        ok = expect(!parses(value), std::string("rejects `") + value + "`") && ok;
    }
    // Every rejected value that contains text names the offending token in its diagnostic; an empty
    // value has nothing to name.
    for (const char* value : invalid) {
        const std::string_view text(value);
        if (text.find_first_not_of(" \t\r\n\f") == std::string_view::npos) {
            continue;
        }
        ok = expect(names_a_token(value), std::string("reports a token for `") + value + "`") && ok;
    }
    // The list bound keeps a stylesheet from making the paint pass fold an unbounded list.
    std::string many;
    for (int index = 0; index < 9; ++index) {
        many += "saturate(1) ";
    }
    ok = expect(!parses(many), "rejects more than kMaxBackdropFilterFunctions") && ok;
    return ok;
}

// --- registry + cascade ----------------------------------------------------------------------

bool registry() {
    const PropertyRegistry registry;
    bool ok =
        expect(registry.supported("backdrop-filter"), "backdrop-filter is a registered property");
    ok = expect(registry.supported("-webkit-backdrop-filter"),
                "-webkit-backdrop-filter is a registered alias") &&
         ok;
    const auto* info = registry.find("backdrop-filter");
    ok = expect(info != nullptr && info->initial == "none" && !info->inherited,
                "initial `none`, not inherited") &&
         ok;

    const auto accepted = registry.expand({"backdrop-filter", "blur(8px) saturate(1.4)", false, 5});
    ok = expect(accepted.diagnostics.empty() && accepted.declarations.size() == 1,
                "a supported value expands to one declaration") &&
         ok;
    ok = expect(!accepted.declarations.empty() &&
                    accepted.declarations.front().property == "backdrop-filter",
                "the canonical property name is kept") &&
         ok;

    const auto alias = registry.expand({"-webkit-backdrop-filter", "blur(8px)", false, 5});
    ok = expect(alias.diagnostics.empty() && alias.declarations.size() == 1 &&
                    alias.declarations.front().property == "backdrop-filter",
                "the webkit alias rewrites to backdrop-filter") &&
         ok;

    const auto unsupported = registry.expand({"backdrop-filter", "blur(8px) sepia(1)", false, 5});
    ok =
        expect(unsupported.declarations.empty(), "an unsupported function drops the declaration") &&
        ok;
    ok = expect(unsupported.diagnostics.size() == 1 &&
                    unsupported.diagnostics.front().kind == StyleDiagnosticKind::UnsupportedValue,
                "the drop is reported as UnsupportedValue") &&
         ok;
    ok = expect(unsupported.diagnostics.size() == 1 &&
                    unsupported.diagnostics.front().message.find("sepia") != std::string::npos,
                "the diagnostic names the offending function") &&
         ok;

    const auto initial = registry.initial_style();
    const std::string* value = initial.find("backdrop-filter");
    ok = expect(value != nullptr && *value == "none", "initial computed value is `none`") && ok;
    const std::string* alias_value = initial.find("-webkit-backdrop-filter");
    ok = expect(alias_value != nullptr && *alias_value == "none",
                "the alias starts at the same initial value") &&
         ok;
    return ok;
}

// --- @supports -------------------------------------------------------------------------------

bool supports_queries() {
    bool ok = expect(supports("(backdrop-filter: blur(1px))"),
                     "@supports (backdrop-filter: blur(1px)) is true");
    ok = expect(supports("(-webkit-backdrop-filter: blur(1px))"),
                "the webkit alias is supported in @supports") &&
         ok;
    ok = expect(supports("(backdrop-filter: none)"), "@supports accepts `none`") && ok;
    ok = expect(supports("(backdrop-filter: blur(1px) saturate(140%))"),
                "@supports follows the full grammar") &&
         ok;
    ok = expect(!supports("(backdrop-filter: sepia(1))"),
                "@supports rejects an unsupported function") &&
         ok;
    ok = expect(!supports("(backdrop-filter: blur(10%))"),
                "@supports rejects an invalid argument") &&
         ok;
    ok = expect(!supports("(backdrop-filter: blur(1px)) and (backdrop-filter: sepia(1))"),
                "a false conjunct makes the query false") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = grammar();
    ok = grammar_rejects() && ok;
    ok = registry() && ok;
    ok = supports_queries() && ok;
    if (!ok) {
        std::cerr << "glass_style: FAILED\n";
        return 1;
    }
    std::cout << "glass_style: ok\n";
    return 0;
}
