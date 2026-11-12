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
using tessera::ui::style::prepare_stylesheet;
using tessera::ui::style::PreparedStyleSheet;
using tessera::ui::style::StyleNode;
using tessera::ui::style::StylesheetSource;

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
    if (!expect(with_at_rule.rules().size() == 2, "media nested compiled") ||
        !expect(with_at_rule.index().size() == 2, "media nested indexed") ||
        !expect(value_is(with_at_rule.compute(button, nullptr), "color", "blue"),
                "top-level rule after at-rule")) {
        return 12;
    }

    const CompiledStyleSheet false_media =
        compile_stylesheet("@media (min-width: 99999px) { .card { color: red; } } "
                           ".card { color: blue; }");
    if (!expect(false_media.rules().size() == 1, "false media skipped") ||
        !expect(value_is(false_media.compute(button, nullptr), "color", "blue"),
                "false media leaves top-level rule")) {
        return 13;
    }

    const CompiledStyleSheet animated =
        compile_stylesheet("@keyframes pulse { from { opacity: 0; } to { opacity: 1; } } "
                           ".card { color: blue; }");
    if (!expect(animated.keyframes().size() == 1, "keyframes compiled") ||
        !expect(animated.keyframes()[0].steps.size() == 2, "keyframes steps") ||
        !expect(animated.rules().size() == 1, "keyframes do not create rules")) {
        return 14;
    }

    const CompiledStyleSheet layered =
        compile_stylesheet("@layer base, components;"
                           "@layer base { .card { color: red; } }"
                           "@layer components { .card { color: blue; } }");
    if (!expect(layered.layer_order().size() == 2, "layer order count") ||
        !expect(layered.rules().size() == 2, "layer rules") ||
        !expect(value_is(layered.compute(button, nullptr), "color", "blue"), "later layer wins")) {
        return 15;
    }

    const CompiledStyleSheet layered_unlayered =
        compile_stylesheet("@layer base, components;"
                           "@layer base { .card { color: red; } }"
                           "@layer components { .card { color: blue; } }"
                           ".card { color: green; }");
    if (!expect(value_is(layered_unlayered.compute(button, nullptr), "color", "green"),
                "unlayered beats layers")) {
        return 16;
    }

    const CompiledStyleSheet layered_important =
        compile_stylesheet("@layer base, components;"
                           "@layer base { .card { color: red !important; } }"
                           "@layer components { .card { color: blue !important; } }");
    if (!expect(value_is(layered_important.compute(button, nullptr), "color", "red"),
                "earlier layer wins important")) {
        return 17;
    }

    const CompiledStyleSheet supports_true =
        compile_stylesheet("@supports (display: flex) { .card { color: red; } }");
    if (!expect(supports_true.rules().size() == 1, "supports true compiles") ||
        !expect(value_is(supports_true.compute(button, nullptr), "color", "red"),
                "supports true style")) {
        return 18;
    }

    const CompiledStyleSheet supports_false =
        compile_stylesheet("@supports (display: grid) { .card { color: red; } }");
    if (!expect(supports_false.rules().empty(), "supports false skips") ||
        !expect(value_is(supports_false.compute(button, nullptr), "color", "black"),
                "supports false keeps initial style")) {
        return 19;
    }

    const CompiledStyleSheet supports_not =
        compile_stylesheet("@supports not (display: grid) { .card { color: green; } }");
    if (!expect(value_is(supports_not.compute(button, nullptr), "color", "green"),
                "supports not")) {
        return 20;
    }

    const CompiledStyleSheet supports_or =
        compile_stylesheet("@supports (display: grid) or (display: flex) {"
                           ".card { color: blue; } }");
    if (!expect(value_is(supports_or.compute(button, nullptr), "color", "blue"), "supports or")) {
        return 21;
    }

    const CompiledStyleSheet supports_invalid =
        compile_stylesheet("@supports () { .card { color: red; } }");
    if (!expect(supports_invalid.rules().empty(), "invalid supports skips") ||
        !expect(supports_invalid.diagnostics().size() == 1, "invalid supports diagnostic") ||
        !expect(supports_invalid.diagnostics()[0].kind ==
                    tessera::ui::style::StyleDiagnosticKind::InvalidAtRule,
                "invalid supports diagnostic kind")) {
        return 22;
    }

    const CompiledStyleSheet empty = compile_stylesheet("");
    if (!expect(empty.rules().empty() && empty.index().size() == 0, "empty stylesheet")) {
        return 23;
    }

    // --- 1. Two app sheets: the later sheet wins an otherwise equal key ------------------------
    {
        StyleNode probe;
        probe.tag = "div";
        probe.classes = {"x"};

        const PreparedStyleSheet red = prepare_stylesheet(".x { color: red; }");
        const PreparedStyleSheet blue = prepare_stylesheet(".x { color: blue; }");
        const StylesheetSource forward[] = {{"a.css", &red}, {"b.css", &blue}};
        const CompiledStyleSheet red_then_blue(forward);
        if (!expect(value_is(red_then_blue.compute(probe, nullptr), "color", "blue"),
                    "the later sheet wins an equal specificity tie") ||
            !expect(red_then_blue.rules().size() == 2 &&
                        red_then_blue.rules()[0].source_order == 0 &&
                        red_then_blue.rules()[1].source_order == 1,
                    "source order is one global increasing sequence across sheets") ||
            !expect(red_then_blue.rules()[1].source_order ==
                        red_then_blue.rules()[0].source_order +
                            red_then_blue.rules()[0].declarations.size(),
                    "the global source order continues across a sheet boundary")) {
            return 24;
        }
        const StylesheetSource reversed[] = {{"b.css", &blue}, {"a.css", &red}};
        const CompiledStyleSheet blue_then_red(reversed);
        if (!expect(value_is(blue_then_red.compute(probe, nullptr), "color", "red"),
                    "reversing the sheet order reverses the winner")) {
            return 25;
        }
    }

    // --- 2. Layers beat specificity: library CSS in @layer tessera-ui ---------------------------
    {
        StyleNode probe;
        probe.tag = "div";
        probe.classes = {"x", "y"};

        const PreparedStyleSheet app = prepare_stylesheet(".x { color: green; }");
        const PreparedStyleSheet library = prepare_stylesheet("div.x.y { color: red; }");
        // The library source comes second in the array and still sorts below the app sheet.
        const StylesheetSource sources[] = {{"app.css", &app},
                                            {"lib.css", &library, Origin::Author, true}};
        const CompiledStyleSheet combined_layers(sources);
        if (!expect(value_is(combined_layers.compute(probe, nullptr), "color", "green"),
                    "unlayered app CSS beats a higher-specificity library rule") ||
            !expect(combined_layers.layer_order().count("tessera-ui") == 1,
                    "library CSS lands in @layer tessera-ui")) {
            return 26;
        }

        // The library's own named layers nest under the library layer.
        const PreparedStyleSheet nested =
            prepare_stylesheet("@layer base { div.x.y { color: red; } }");
        const StylesheetSource nested_sources[] = {{"app.css", &app},
                                                   {"lib.css", &nested, Origin::Author, true}};
        const CompiledStyleSheet nested_combined(nested_sources);
        if (!expect(value_is(nested_combined.compute(probe, nullptr), "color", "green"),
                    "a library rule inside its own layer loses to unlayered app CSS") ||
            !expect(nested_combined.layer_order().count("tessera-ui.base") == 1,
                    "a library layer nests under tessera-ui") ||
            !expect(nested_combined.layer_order().at("tessera-ui") == 0,
                    "tessera-ui is the lowest layer slot")) {
            return 27;
        }

        // Existing named-layer semantics survive the combination: an app rule inside a layer loses
        // to an unlayered app rule, and a layered app rule still beats the library layer.
        const PreparedStyleSheet layered_app =
            prepare_stylesheet("@layer app-base { div.x.y { color: purple; } }");
        const StylesheetSource layered_sources[] = {{"app.css", &layered_app},
                                                    {"lib.css", &library, Origin::Author, true}};
        const CompiledStyleSheet layered_combined(layered_sources);
        const PreparedStyleSheet plain_app = prepare_stylesheet(".x { color: green; }");
        const StylesheetSource mixed_sources[] = {{"layered.css", &layered_app},
                                                  {"plain.css", &plain_app}};
        const CompiledStyleSheet mixed(mixed_sources);
        if (!expect(value_is(layered_combined.compute(probe, nullptr), "color", "purple"),
                    "a layered app rule beats the library layer") ||
            !expect(value_is(mixed.compute(probe, nullptr), "color", "green"),
                    "an unlayered app rule beats a layered app rule")) {
            return 28;
        }
    }

    // --- 3. Keyframes: the first definition of a name wins --------------------------------------
    {
        const PreparedStyleSheet first =
            prepare_stylesheet("@keyframes pulse { from { opacity: 0; } to { opacity: 1; } }");
        const PreparedStyleSheet second =
            prepare_stylesheet("@keyframes pulse { from { opacity: 1; } to { opacity: 0; } } "
                               "@keyframes fade { from { opacity: 0; } }");
        const StylesheetSource sources[] = {{"a.css", &first}, {"b.css", &second}};
        const CompiledStyleSheet combined_keyframes(sources);
        if (!expect(combined_keyframes.keyframes().size() == 2,
                    "a repeated name is merged, not duplicated") ||
            !expect(combined_keyframes.keyframes()[0].name == "pulse" &&
                        combined_keyframes.keyframes()[0].steps.size() == 2 &&
                        combined_keyframes.keyframes()[0].steps[0].declarations[0].value == "0",
                    "the first definition of a keyframes name wins") ||
            !expect(combined_keyframes.keyframes()[1].name == "fade", "a new name is appended")) {
            return 29;
        }
    }

    // --- 4. Import-graph order: three sheets apply in exactly the given order -------------------
    {
        StyleNode probe;
        probe.tag = "div";
        probe.classes = {"x"};

        const PreparedStyleSheet first = prepare_stylesheet(".x { width: 1px; color: red; }");
        const PreparedStyleSheet second = prepare_stylesheet(".x { width: 2px; color: green; }");
        const PreparedStyleSheet third =
            prepare_stylesheet(".x { width: 3px; color: blue; not-a-property: 1; }");
        const StylesheetSource sources[] = {
            {"a.css", &first}, {"b.css", &second}, {"c.css", &third}};
        const CompiledStyleSheet combined_order(sources);
        if (!expect(value_is(combined_order.compute(probe, nullptr), "width", "3px"),
                    "the third sheet wins width") ||
            !expect(value_is(combined_order.compute(probe, nullptr), "color", "blue"),
                    "the third sheet wins color") ||
            !expect(combined_order.rules().size() == 3 &&
                        combined_order.rules()[0].source_order <
                            combined_order.rules()[1].source_order &&
                        combined_order.rules()[1].source_order <
                            combined_order.rules()[2].source_order,
                    "three sheets keep the given order") ||
            !expect(combined_order.diagnostics().size() == 1,
                    "diagnostics concatenate across sheets")) {
            return 30;
        }
    }

    std::cout << "compiled_stylesheet: ok\n";
    return 0;
}
