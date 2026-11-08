// SPDX-License-Identifier: MIT

#include "tessera/ui/style/cascade.hpp"

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

    std::cout << "cascade: ok\n";
    return 0;
}
