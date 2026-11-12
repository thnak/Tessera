// SPDX-License-Identifier: MIT

#include "tessera/ui/style/cascade.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::ui::style::CascadeDeclaration;
using tessera::ui::style::CascadeKey;
using tessera::ui::style::cascades_over;
using tessera::ui::style::CascadeTable;
using tessera::ui::style::compute_style;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::Origin;
using tessera::ui::style::Specificity;
using tessera::ui::style::StyleDiagnostic;
using tessera::ui::style::StyleDiagnosticKind;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cascade: " << message << "\n";
    }
    return condition;
}

CascadeDeclaration decl(std::string property, std::string value, Origin origin, std::uint64_t order,
                        Specificity specificity = {}, bool important = false,
                        std::uint32_t layer = tessera::ui::style::kUnlayeredLayer) {
    return CascadeDeclaration{
        std::move(property), std::move(value), important, origin, specificity, order, layer};
}

const std::string* value_of(const ComputedStyle& style, const char* property) {
    return style.find(property);
}

} // namespace

int main() {
    CascadeTable table;
    table.add(decl("color", "ua", Origin::UserAgent, 1));
    table.add(decl("color", "author", Origin::Author, 2));
    table.add(decl("color", "user", Origin::User, 3));
    if (!expect(table.winner("color") != nullptr, "normal winner") ||
        !expect(table.winner("color")->value == "author", "author beats user agent/user")) {
        return 1;
    }

    table.clear();
    table.add(decl("color", "author", Origin::Author, 1));
    table.add(decl("color", "inline", Origin::Inline, 2));
    if (!expect(table.winner("color")->value == "inline", "inline beats author")) {
        return 2;
    }

    table.clear();
    table.add(decl("color", "author", Origin::Author, 1));
    table.add(decl("color", "user-important", Origin::User, 2, {}, true));
    table.add(decl("color", "author-normal", Origin::Author, 3));
    if (!expect(table.winner("color")->value == "user-important", "important beats normal")) {
        return 3;
    }

    table.add(decl("color", "ua-important", Origin::UserAgent, 4, {}, true));
    if (!expect(table.winner("color")->value == "ua-important",
                "important user agent beats important user")) {
        return 4;
    }

    table.clear();
    table.add(decl("color", "author-important", Origin::Author, 1, {}, true));
    table.add(decl("color", "inline-important", Origin::Inline, 2, {}, true));
    if (!expect(table.winner("color")->value == "inline-important",
                "important inline beats important author")) {
        return 5;
    }

    table.clear();
    table.add(decl("color", "class", Origin::Author, 1, Specificity{0, 1, 0}));
    table.add(decl("color", "id", Origin::Author, 2, Specificity{1, 0, 0}));
    table.add(decl("color", "type", Origin::Author, 3, Specificity{0, 0, 1}));
    if (!expect(table.winner("color")->value == "id", "specificity beats source order")) {
        return 6;
    }

    table.clear();
    table.add(decl("color", "first", Origin::Author, 1, Specificity{0, 1, 0}));
    table.add(decl("color", "later", Origin::Author, 2, Specificity{0, 1, 0}));
    if (!expect(table.winner("color")->value == "later", "source order breaks ties")) {
        return 7;
    }

    table.clear();
    table.add(decl("Color", "red", Origin::Author, 1));
    table.add(decl("color", "blue", Origin::Author, 2));
    table.add(decl("--X", "one", Origin::Author, 3));
    table.add(decl("--x", "two", Origin::Author, 4));
    if (!expect(table.size() == 3, "property normalization") ||
        !expect(table.winner("COLOR")->value == "blue", "normalized lookup") ||
        !expect(table.winner("--X")->value == "one", "custom property case") ||
        !expect(table.winner("--x")->value == "two", "custom property distinct")) {
        return 8;
    }

    table.clear();
    table.add(decl("z-index", "1", Origin::Author, 1));
    table.add(decl("align-items", "center", Origin::Author, 2));
    const auto winners = table.winners();
    if (!expect(winners.size() == 2, "winners size") ||
        !expect(winners[0].property == "align-items", "winners sorted") ||
        !expect(winners[1].property == "z-index", "winners sorted second")) {
        return 9;
    }

    table.clear();
    std::vector<Declaration> declarations = {
        Declaration{"color", "red", false, 0},
        Declaration{"opacity", "0.5", true, 10},
    };
    std::uint64_t source_order = 0;
    table.add_declarations(declarations, Origin::Author, Specificity{0, 1, 0}, source_order);
    if (!expect(source_order == 2, "source order advances") ||
        !expect(table.size() == 2, "declarations added") ||
        !expect(table.winner("color")->value == "red", "declaration color") ||
        !expect(table.winner("opacity")->important, "declaration important")) {
        return 10;
    }

    const CascadeKey older{Origin::Author, false, Specificity{0, 1, 0}, 5};
    const CascadeKey newer{Origin::Author, false, Specificity{0, 1, 0}, 6};
    if (!expect(cascades_over(newer, older), "key comparison") ||
        !expect(!cascades_over(older, newer), "key comparison reverse")) {
        return 11;
    }

    ComputedStyle parent;
    parent.set("color", "red");
    parent.set("font-size", "16px");
    parent.set("font-feature-settings", "'liga'");
    parent.set("width", "100px");
    parent.set("--token", "1");

    CascadeTable child;
    child.add(decl("color", "blue", Origin::Author, 1, Specificity{0, 1, 0}));
    child.add(decl("width", "10px", Origin::Author, 2));
    child.add(decl("height", "20px", Origin::Author, 3));
    const ComputedStyle computed = compute_style(&parent, child);
    if (!expect(value_of(computed, "color") != nullptr && *value_of(computed, "color") == "blue",
                "child color") ||
        !expect(value_of(computed, "font-size") != nullptr &&
                    *value_of(computed, "font-size") == "16px",
                "inherited font-size") ||
        !expect(value_of(computed, "font-feature-settings") != nullptr, "inherited font-*") ||
        !expect(value_of(computed, "--token") != nullptr && *value_of(computed, "--token") == "1",
                "inherited custom property") ||
        !expect(value_of(computed, "width") != nullptr && *value_of(computed, "width") == "10px",
                "child width overrides") ||
        !expect(value_of(computed, "height") != nullptr && *value_of(computed, "height") == "20px",
                "child height")) {
        return 12;
    }

    CascadeTable inherit_table;
    inherit_table.add(decl("color", "inherit", Origin::Author, 1));
    inherit_table.add(decl("width", "inherit", Origin::Author, 2));
    ComputedStyle inherited = compute_style(&parent, inherit_table);
    if (!expect(value_of(inherited, "color") != nullptr && *value_of(inherited, "color") == "red",
                "inherit color") ||
        !expect(value_of(inherited, "width") != nullptr && *value_of(inherited, "width") == "100px",
                "inherit non-inherited property")) {
        return 13;
    }

    CascadeTable initial_table;
    initial_table.add(decl("color", "initial", Origin::Author, 1));
    initial_table.add(decl("width", "initial", Origin::Author, 2));
    const ComputedStyle initial = compute_style(&parent, initial_table);
    if (!expect(!initial.contains("color"), "initial clears inherited") ||
        !expect(!initial.contains("width"), "initial clears non-inherited")) {
        return 14;
    }

    CascadeTable unset_table;
    unset_table.add(decl("color", "unset", Origin::Author, 1));
    unset_table.add(decl("width", "unset", Origin::Author, 2));
    const ComputedStyle unset = compute_style(&parent, unset_table);
    if (!expect(value_of(unset, "color") != nullptr && *value_of(unset, "color") == "red",
                "unset inherited") ||
        !expect(!unset.contains("width"), "unset non-inherited")) {
        return 15;
    }

    CascadeTable revert_table;
    revert_table.add(decl("color", "revert", Origin::Author, 1));
    const ComputedStyle reverted = compute_style(&parent, revert_table);
    if (!expect(value_of(reverted, "color") != nullptr && *value_of(reverted, "color") == "red",
                "revert treated as unset")) {
        return 16;
    }

    const ComputedStyle no_parent = compute_style(nullptr, inherit_table);
    if (!expect(!no_parent.contains("color"), "inherit without parent clears")) {
        return 17;
    }

    table.clear();
    table.add(decl("color", "user", Origin::User, 1));
    table.add(decl("color", "theme", Origin::Theme, 2));
    if (!expect(table.winner("color")->value == "theme", "theme beats user")) {
        return 18;
    }
    table.add(decl("color", "author", Origin::Author, 3));
    if (!expect(table.winner("color")->value == "author", "author beats theme")) {
        return 19;
    }

    table.clear();
    table.add(decl("color", "base", Origin::Author, 1, {}, false, 0));
    table.add(decl("color", "components", Origin::Author, 2, {}, false, 1));
    if (!expect(table.winner("color")->value == "components", "later layer wins normal")) {
        return 20;
    }
    table.add(decl("color", "unlayered", Origin::Author, 3));
    if (!expect(table.winner("color")->value == "unlayered", "unlayered beats layers normal")) {
        return 21;
    }

    table.clear();
    table.add(decl("color", "base-important", Origin::Author, 1, {}, true, 0));
    table.add(decl("color", "components-important", Origin::Author, 2, {}, true, 1));
    if (!expect(table.winner("color")->value == "base-important", "earlier layer wins important")) {
        return 22;
    }

    table.clear();
    table.add(decl("color", "layered-important", Origin::Author, 1, {}, true, 0));
    table.add(decl("color", "unlayered-important", Origin::Author, 2, {}, true));
    if (!expect(table.winner("color")->value == "layered-important",
                "layered important beats unlayered important")) {
        return 23;
    }

    const auto count_kind = [](const std::vector<StyleDiagnostic>& diagnostics,
                               StyleDiagnosticKind kind) {
        return std::count_if(
            diagnostics.begin(), diagnostics.end(),
            [kind](const StyleDiagnostic& diagnostic) { return diagnostic.kind == kind; });
    };

    // var(): computed-value substitution covers a bare reference, a fallback, a nested reference,
    // case-insensitive and whitespace-padded syntax, and never rewrites a quoted string.
    CascadeTable var_table;
    var_table.add(decl("--accent", "#ff0000", Origin::Author, 1));
    var_table.add(decl("--tx", "1px", Origin::Author, 2));
    var_table.add(decl("--ty", "2px", Origin::Author, 3));
    var_table.add(decl("--chain", "var(--tx)", Origin::Author, 4));
    var_table.add(decl("color", "var(--accent)", Origin::Author, 5));
    var_table.add(decl("border-top-width", "var(--missing, 2px)", Origin::Author, 6));
    var_table.add(decl("transform", "translate(var(--chain), var(--ty))", Origin::Author, 7));
    var_table.add(decl("letter-spacing", "VAR( --accent )", Origin::Author, 8));
    var_table.add(decl("font-family", "\"var(--accent)\"", Origin::Author, 9));
    std::vector<StyleDiagnostic> var_diagnostics;
    const ComputedStyle var_resolved = compute_style(nullptr, var_table, &var_diagnostics);
    if (!expect(value_of(var_resolved, "color") != nullptr &&
                    *value_of(var_resolved, "color") == "#ff0000",
                "var resolves to the custom property value") ||
        !expect(value_of(var_resolved, "border-top-width") != nullptr &&
                    *value_of(var_resolved, "border-top-width") == "2px",
                "var fallback applies when the reference is missing") ||
        !expect(value_of(var_resolved, "transform") != nullptr &&
                    *value_of(var_resolved, "transform") == "translate(1px, 2px)",
                "nested var and multiple references substitute") ||
        !expect(value_of(var_resolved, "letter-spacing") != nullptr &&
                    *value_of(var_resolved, "letter-spacing") == "#ff0000",
                "var name is trimmed and matched case-insensitively") ||
        !expect(value_of(var_resolved, "font-family") != nullptr &&
                    *value_of(var_resolved, "font-family") == "\"var(--accent)\"",
                "var inside a quoted string is literal") ||
        !expect(var_diagnostics.empty(), "successful resolution reports no diagnostic")) {
        return 24;
    }

    // Custom properties inherit, so a child resolves a reference to a parent token.
    ComputedStyle var_parent;
    var_parent.set("--accent", "blue");
    var_parent.set("color", "green");
    CascadeTable inherited_var_table;
    inherited_var_table.add(decl("color", "var(--accent)", Origin::Author, 1));
    inherited_var_table.add(decl("width", "var(--gap, 5px)", Origin::Author, 2));
    const ComputedStyle inherited_var = compute_style(&var_parent, inherited_var_table);
    if (!expect(value_of(inherited_var, "color") != nullptr &&
                    *value_of(inherited_var, "color") == "blue",
                "inherited custom property resolves") ||
        !expect(value_of(inherited_var, "width") != nullptr &&
                    *value_of(inherited_var, "width") == "5px",
                "fallback applies in a child")) {
        return 25;
    }

    // Invalid at computed-value time behaves like `unset`: inherited properties keep the parent
    // value and every other property is dropped for the initial-value pass.
    ComputedStyle unset_parent;
    unset_parent.set("color", "green");
    CascadeTable unresolved_table;
    unresolved_table.add(decl("color", "var(--missing)", Origin::Author, 1));
    unresolved_table.add(decl("width", "var(--missing)", Origin::Author, 2));
    std::vector<StyleDiagnostic> unresolved_diagnostics;
    const ComputedStyle unresolved =
        compute_style(&unset_parent, unresolved_table, &unresolved_diagnostics);
    if (!expect(value_of(unresolved, "color") != nullptr &&
                    *value_of(unresolved, "color") == "green",
                "unresolved var inherits the parent value") ||
        !expect(!unresolved.contains("width"), "unresolved var drops a non-inherited property") ||
        !expect(count_kind(unresolved_diagnostics, StyleDiagnosticKind::UnresolvedVar) == 2,
                "each unresolved reference reports once")) {
        return 26;
    }

    // A cycle invalidates every member; a consumer falls back instead of inheriting a cycle value.
    CascadeTable cycle_table;
    cycle_table.add(decl("--a", "var(--b)", Origin::Author, 1));
    cycle_table.add(decl("--b", "var(--a)", Origin::Author, 2));
    cycle_table.add(decl("color", "var(--a, teal)", Origin::Author, 3));
    std::vector<StyleDiagnostic> cycle_diagnostics;
    const ComputedStyle cycle = compute_style(nullptr, cycle_table, &cycle_diagnostics);
    if (!expect(!cycle.contains("--a") && !cycle.contains("--b"), "cycle members are invalid") ||
        !expect(value_of(cycle, "color") != nullptr && *value_of(cycle, "color") == "teal",
                "cycle consumer uses the fallback") ||
        !expect(count_kind(cycle_diagnostics, StyleDiagnosticKind::VarCycle) == 2,
                "every cycle member reports once")) {
        return 27;
    }

    // A self-reference is a cycle too, and a custom property with an unresolved reference is
    // guaranteed-invalid for its consumers.
    CascadeTable self_cycle_table;
    self_cycle_table.add(decl("--self", "var(--self)", Origin::Author, 1));
    self_cycle_table.add(decl("background-color", "var(--self, orange)", Origin::Author, 2));
    self_cycle_table.add(decl("--broken", "var(--missing)", Origin::Author, 3));
    self_cycle_table.add(decl("outline", "var(--broken, 3px)", Origin::Author, 4));
    std::vector<StyleDiagnostic> self_cycle_diagnostics;
    const ComputedStyle self_cycle =
        compute_style(nullptr, self_cycle_table, &self_cycle_diagnostics);
    if (!expect(!self_cycle.contains("--self"), "self reference is invalid") ||
        !expect(value_of(self_cycle, "background-color") != nullptr &&
                    *value_of(self_cycle, "background-color") == "orange",
                "self-reference consumer uses the fallback") ||
        !expect(!self_cycle.contains("--broken"), "unresolved custom property is invalid") ||
        !expect(count_kind(self_cycle_diagnostics, StyleDiagnosticKind::VarCycle) == 1,
                "self reference reports one cycle") ||
        !expect(count_kind(self_cycle_diagnostics, StyleDiagnosticKind::UnresolvedVar) == 1,
                "unresolved custom property reports once")) {
        return 28;
    }

    std::cout << "cascade: ok\n";
    return 0;
}
