// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::CascadeTable;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::compute_style;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Origin;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "compiled_stylesheet: " << message << "\n";
    }
    return condition;
}

void attach(StyleNode& parent, StyleNode& child) {
    child.parent = &parent;
    parent.children.push_back(&child);
}

bool value_is(const ComputedStyle& style, const char* property, std::string_view expected) {
    const std::string* value = style.find(property);
    return value != nullptr && *value == expected;
}

} // namespace

int main() {
    StyleNode root;
    root.tag = "root";

    StyleNode button;
    button.tag = "button";
    button.id = "save";
    button.classes = {"card"};

    StyleNode title;
    title.tag = "span";
    title.classes = {"title"};

    attach(root, button);
    attach(button, title);

    constexpr std::string_view source = R"(
button { color: red; }
.card { color: blue; padding: 4px; }
#save { color: green; font-size: 14px; }
.card .title { color: purple; }
button { padding: 1px; }
)";
    const CompiledStyleSheet compiled = compile_stylesheet(source);
    if (!expect(compiled.origin() == Origin::Author, "default origin") ||
        !expect(compiled.rules().size() == 5, "compiled rule count") ||
        !expect(compiled.index().size() == 5, "indexed selector count")) {
        return 1;
    }

    const CascadeTable button_cascade = compiled.cascade(button);
    if (!expect(button_cascade.winner("color") != nullptr &&
                    button_cascade.winner("color")->value == "green",
                "id wins color") ||
        !expect(button_cascade.winner("padding-top") != nullptr &&
                    button_cascade.winner("padding-top")->value == "4px",
                "class beats type for padding") ||
        !expect(button_cascade.winner("font-size") != nullptr &&
                    button_cascade.winner("font-size")->value == "14px",
                "font-size matched")) {
        return 2;
    }

    const ComputedStyle button_style = compiled.compute(button, nullptr);
    if (!expect(value_is(button_style, "color", "green"), "computed color") ||
        !expect(value_is(button_style, "padding-top", "4px"), "computed padding") ||
        !expect(value_is(button_style, "font-size", "14px"), "computed font-size")) {
        return 3;
    }

    const ComputedStyle title_style = compiled.compute(title, &button_style);
    if (!expect(value_is(title_style, "color", "purple"), "descendant color") ||
        !expect(value_is(title_style, "font-size", "14px"), "inherited font-size") ||
        !expect(value_is(title_style, "padding-top", "0"), "non-inherited padding initial")) {
        return 4;
    }

    constexpr std::string_view important_source =
        ".card { color: red !important; } #save { color: blue; }";
    const CompiledStyleSheet important = compile_stylesheet(important_source);
    const ComputedStyle important_style = important.compute(button, nullptr);
    if (!expect(value_is(important_style, "color", "red"), "important beats id")) {
        return 5;
    }

    const CompiledStyleSheet ua = compile_stylesheet(".card { color: red; }", Origin::UserAgent);
    const CompiledStyleSheet author = compile_stylesheet(".card { color: blue; }", Origin::Author);
    CascadeTable combined;
    ua.add_to_cascade(button, combined);
    author.add_to_cascade(button, combined);
    const ComputedStyle combined_style = compute_style(nullptr, combined);
    if (!expect(value_is(combined_style, "color", "blue"), "author beats user agent")) {
        return 6;
    }

    combined.clear();
    const CompiledStyleSheet important_ua =
        compile_stylesheet(".card { color: red !important; }", Origin::UserAgent);
    important_ua.add_to_cascade(button, combined);
    author.add_to_cascade(button, combined);
    const ComputedStyle important_ua_style = compute_style(nullptr, combined);
    if (!expect(value_is(important_ua_style, "color", "red"),
                "important user agent beats author")) {
        return 7;
    }

    const CompiledStyleSheet list =
        compile_stylesheet(".card, #save { color: teal; }", Origin::Author);
    const CascadeTable list_cascade = list.cascade(button);
    if (!expect(list.rules().size() == 1, "selector list rule") ||
        !expect(list.index().size() == 2, "selector list index") ||
        !expect(list_cascade.winner("color") != nullptr &&
                    list_cascade.winner("color")->specificity.ids == 1,
                "selector list max specificity")) {
        return 8;
    }
    const ComputedStyle list_style = list.compute(button, nullptr);
    if (!expect(value_is(list_style, "color", "teal"), "selector list value")) {
        return 9;
    }

    const CompiledStyleSheet ordered =
        compile_stylesheet(".card { color: red; } .card { color: blue; }");
    if (!expect(value_is(ordered.compute(button, nullptr), "color", "blue"), "source order wins")) {
        return 10;
    }

    const CompiledStyleSheet recovered =
        compile_stylesheet("..bad { color: red; } .card { color: blue; }");
    if (!expect(recovered.rules().size() == 1, "invalid selector skipped") ||
        !expect(recovered.index().size() == 1, "invalid selector not indexed") ||
        !expect(value_is(recovered.compute(button, nullptr), "color", "blue"),
                "recovered rule applies")) {
        return 11;
    }

    const CompiledStyleSheet with_at_rule =
        compile_stylesheet("@media (min-width: 1px) { .card { color: red; } } "
                           ".card { color: blue; }");
    if (!expect(with_at_rule.rules().size() == 1, "at-rule not compiled") ||
        !expect(value_is(with_at_rule.compute(button, nullptr), "color", "blue"),
                "top-level rule after at-rule")) {
        return 12;
    }

    const CompiledStyleSheet empty = compile_stylesheet("");
    if (!expect(empty.rules().empty() && empty.index().size() == 0, "empty stylesheet")) {
        return 13;
    }

    std::cout << "compiled_stylesheet: ok\n";
    return 0;
}
