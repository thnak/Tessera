// SPDX-License-Identifier: MIT
//
// `env()` (ADR-074 section 2): fallback and invalid-at-computed-value-time rules, the closed list
// of standard variables, and the "re-resolve only when a name this sheet reads changed" answer.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/style_environment.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace tessera::ui::style;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "style_environment: " << message << "\n";
    }
    return condition;
}

ComputedStyle compute(const std::vector<std::pair<std::string, std::string>>& declarations,
                      const StyleEnvironment* environment,
                      std::vector<StyleDiagnostic>* diagnostics = nullptr) {
    CascadeTable table;
    std::uint64_t order = 0;
    std::vector<Declaration> list;
    for (const auto& [property, value] : declarations) {
        list.push_back(Declaration{property, value, false});
    }
    table.add_declarations(list, Origin::Author, Specificity{}, order);
    return compute_style(nullptr, table, diagnostics, environment);
}

std::string value_of(const ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    return value != nullptr ? *value : std::string("<none>");
}

StandardEnvironmentInputs phone_inputs() {
    StandardEnvironmentInputs inputs;
    inputs.safe_area_top = 47.0f;
    inputs.safe_area_bottom = 34.5f;
    inputs.text_scale = 1.25f;
    inputs.viewport_width = 400.0f;
    inputs.viewport_height = 800.0f;
    return inputs;
}

bool test_standard_variables() {
    const StyleEnvironment env = standard_environment(phone_inputs());
    const auto get = [&](const char* name) {
        const std::string* value = env.find(name);
        return value != nullptr ? *value : std::string("<none>");
    };
    StandardEnvironmentInputs fold = phone_inputs();
    fold.viewport_width = 600.0f;
    fold.viewport_height = 400.0f;
    fold.segments_x = 2;
    fold.segments_y = 1;
    fold.segments = {{0.0f, 0.0f, 290.0f, 400.0f}, {310.0f, 0.0f, 290.0f, 400.0f}};
    const StyleEnvironment folded = standard_environment(fold);
    const auto fget = [&](const char* name) {
        const std::string* value = folded.find(name);
        return value != nullptr ? *value : std::string("<none>");
    };
    StandardEnvironmentInputs broken = fold;
    broken.segments.pop_back(); // grid does not match its rectangles
    const StyleEnvironment fallback = standard_environment(broken);
    return expect(get("safe-area-inset-top") == "47px", "inset top") &&
           expect(get("safe-area-inset-bottom") == "34.5px",
                  "fractional inset keeps its decimals") &&
           expect(get("safe-area-inset-left") == "0px", "unset inset is 0px, never -0") &&
           expect(get("preferred-text-scale") == "1.25", "text scale is unitless") &&
           expect(get("viewport-segment-width 0 0") == "400px", "no fold is one whole segment") &&
           expect(get("viewport-segment-right 0 0") == "400px", "right is left plus width") &&
           expect(get("viewport-segment-width 1 0") == "<none>",
                  "no second segment without a fold") &&
           expect(fget("viewport-segment-left 1 0") == "310px", "second segment left") &&
           expect(fget("viewport-segment-right 1 0") == "600px", "second segment right") &&
           expect(fget("viewport-segment-bottom 0 0") == "400px", "segment bottom") &&
           expect(fallback.find("viewport-segment-width 1 0") == nullptr,
                  "a mismatched grid answers as one segment") &&
           expect(get("hinge-angle") == "<none>" && get("--tessera-hinge-angle") == "<none>",
                  "no name outside the closed list");
}

bool test_substitution() {
    const StyleEnvironment env = standard_environment(phone_inputs());
    const ComputedStyle style =
        compute({{"padding-top", "env(safe-area-inset-top)"},
                 {"padding-bottom", "calc(env(safe-area-inset-bottom) + 8px)"},
                 {"width", "env(viewport-segment-width   0 0)"},
                 {"margin-left", "env(safe-area-inset-left, 99px)"},
                 {"margin-right", "env(not-a-variable, 12px)"},
                 {"margin-top", "env(not-a-variable, var(--gap, 3px))"},
                 {"--inset", "env(safe-area-inset-top)"},
                 {"top", "var(--inset)"},
                 {"font-family", "\"env(safe-area-inset-top)\""}},
                &env);
    return expect(value_of(style, "padding-top") == "47px", "env substitutes") &&
           expect(value_of(style, "padding-bottom") == "calc(34.5px + 8px)", "env inside calc") &&
           expect(value_of(style, "width") == "400px", "argument whitespace is normalised") &&
           expect(value_of(style, "margin-left") == "0px",
                  "an existing variable ignores its fallback") &&
           expect(value_of(style, "margin-right") == "12px",
                  "a missing variable takes the fallback") &&
           expect(value_of(style, "margin-top") == "3px", "a fallback may hold var()") &&
           expect(value_of(style, "top") == "47px", "a custom property may hold env()") &&
           expect(value_of(style, "font-family") == "\"env(safe-area-inset-top)\"",
                  "a string is not a call");
}

bool test_invalid_at_computed_value_time() {
    std::vector<StyleDiagnostic> diagnostics;
    const StyleEnvironment env = standard_environment(phone_inputs());
    const ComputedStyle style =
        compute({{"padding-top", "env(nope)"}, {"color", "env(nope)"}}, &env, &diagnostics);
    // No environment at all behaves like an environment with no variables.
    const ComputedStyle bare = compute({{"padding-top", "env(safe-area-inset-top, 5px)"}}, nullptr);
    const ComputedStyle bare_no_fallback =
        compute({{"padding-top", "env(safe-area-inset-top)"}}, nullptr);
    return expect(style.find("padding-top") == nullptr || *style.find("padding-top") != "env(nope)",
                  "an unresolvable env() does not survive") &&
           expect(diagnostics.size() == 2, "one diagnostic per dropped declaration") &&
           expect(!diagnostics.empty() && diagnostics[0].kind == StyleDiagnosticKind::UnresolvedVar,
                  "reported as an unresolved reference") &&
           expect(value_of(bare, "padding-top") == "5px", "null environment takes the fallback") &&
           expect(bare_no_fallback.find("padding-top") == nullptr ||
                      *bare_no_fallback.find("padding-top") != "env(safe-area-inset-top)",
                  "null environment with no fallback is invalid");
}

bool test_parse_time_validity() {
    // A syntactically valid env() keeps the declaration for properties that validate their value at
    // parse time; judging the text before substitution would drop it.
    const PreparedStyleSheet prepared =
        prepare_stylesheet(".a { text-decoration-thickness: env(safe-area-inset-top); "
                           "list-style-type: env(x, disc); }");
    std::size_t kept = 0;
    for (const PreparedItem& item : prepared.items) {
        kept += item.declarations.size();
    }
    return expect(kept == 2 && prepared.diagnostics.empty(), "env() declarations survive parsing");
}

bool test_sheet_reads_env_through_compiled_sheet() {
    CompiledStyleSheet sheet{parse_stylesheet("a { padding-top: env(safe-area-inset-top, 1px); }")};
    StyleNode node;
    node.tag = "a";
    const ComputedStyle before = sheet.compute(node, nullptr);
    sheet.set_environment(
        std::make_shared<const StyleEnvironment>(standard_environment(phone_inputs())));
    const ComputedStyle after = sheet.compute(node, nullptr);
    return expect(value_of(before, "padding-top") == "1px", "no environment: fallback") &&
           expect(value_of(after, "padding-top") == "47px", "environment set: value");
}

bool test_answer_can_change() {
    const PreparedStyleSheet uses_top =
        prepare_stylesheet(".a { padding-top: env(safe-area-inset-top, 0px); }");
    const PreparedStyleSheet uses_nothing = prepare_stylesheet(".a { padding-top: 4px; }");
    const PreparedStyleSheet in_fallback =
        prepare_stylesheet(".a { padding-top: env(missing, env(safe-area-inset-left)); --x: "
                           "env(preferred-text-scale); }");
    StandardEnvironmentInputs base = phone_inputs();
    StandardEnvironmentInputs moved_top = base;
    moved_top.safe_area_top = 20.0f;
    StandardEnvironmentInputs moved_bottom = base;
    moved_bottom.safe_area_bottom = 0.0f;
    StandardEnvironmentInputs rescaled = base;
    rescaled.text_scale = 2.0f;
    const StyleEnvironment a = standard_environment(base);
    const StyleEnvironment top = standard_environment(moved_top);
    const StyleEnvironment bottom = standard_environment(moved_bottom);
    const StyleEnvironment scale = standard_environment(rescaled);
    return expect(uses_top.environment_answer_can_change(a, top), "the read name changed") &&
           expect(!uses_top.environment_answer_can_change(a, bottom),
                  "a name this sheet does not read changed") &&
           expect(!uses_top.environment_answer_can_change(a, a), "equal environments") &&
           expect(!uses_nothing.environment_answer_can_change(a, top) &&
                      !uses_nothing.environment_answer_can_change(a, scale),
                  "a sheet with no env() never re-resolves") &&
           expect(in_fallback.environment_answer_can_change(a, scale),
                  "a custom property's env() counts") &&
           expect(!in_fallback.environment_answer_can_change(a, top),
                  "a name only in a different sheet's property does not") &&
           expect(uses_top.environment_answer_can_change(StyleEnvironment{}, a),
                  "a variable that appears changes the answer");
}

} // namespace

int main() {
    return test_standard_variables() && test_substitution() &&
                   test_invalid_at_computed_value_time() && test_parse_time_validity() &&
                   test_sheet_reads_env_through_compiled_sheet() && test_answer_can_change()
               ? 0
               : 1;
}
