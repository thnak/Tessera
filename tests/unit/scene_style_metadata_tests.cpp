// SPDX-License-Identifier: MIT
//
// Scene element metadata -> selector view: type, id, class and :hover matching against the metadata
// a scene node already stores (docs/design/scene-graph.md section 5). Before this slice the style
// tree exposed tree structure and pseudo-state only, so a compiled stylesheet could not style a
// generated component at all.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::PseudoState;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_style_metadata: " << message << "\n";
    }
    return condition;
}

std::string_view value_of(const ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    return value != nullptr ? std::string_view(*value) : std::string_view{};
}

} // namespace

int main() {
    Node root(1);
    root.set_element_kind("View");

    auto button_ptr = std::make_unique<Node>(2);
    Node& button = root.add_child(std::move(button_ptr));
    button.set_element_kind("Button");
    button.set_element_id("go");
    button.set_class_name("primary  wide\t");

    auto label_ptr = std::make_unique<Node>(3);
    Node& label = button.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    PaintData text;
    text.kind = tessera::ui::PaintKind::Text;
    text.text = "Add";
    label.set_paint_data(std::move(text));

    const auto sheet = compile_stylesheet(R"(
Button { width: 96px; height: 28px; }
#go { border-top-width: 2px; }
.primary { color: red; }
.wide { margin-left: 8px; }
Button.primary:hover { color: green; }
Text { font-size: 14px; }
.secondary { color: blue; }
#missing { color: blue; }
)");
    SceneStyleTree tree(root);

    const auto* button_view = tree.view(button);
    const auto* label_view = tree.view(label);
    if (!expect(button_view != nullptr, "button has a style view") ||
        !expect(label_view != nullptr, "label has a style view") ||
        !expect(button_view->tag == "Button", "tag comes from element_kind") ||
        !expect(button_view->id == "go", "id comes from element_id") ||
        !expect(button_view->classes.size() == 2, "class_name splits on runs of whitespace") ||
        !expect(button_view->classes[0] == "primary" && button_view->classes[1] == "wide",
                "class tokens are in source order") ||
        !expect(!button_view->has_text, "Button has no text payload") ||
        !expect(label_view->has_text, "Text payload sets has_text") ||
        !expect(tree.view(button) == button_view, "view pointers are stable across lookups")) {
        return 1;
    }

    // Type, id and class selectors all resolve against scene metadata.
    const ComputedStyle button_style = tree.compute(sheet, button);
    if (!expect(value_of(button_style, "width") == "96px", "type selector matches element_kind") ||
        !expect(value_of(button_style, "height") == "28px",
                "type selector sets every declaration") ||
        !expect(value_of(button_style, "border-top-width") == "2px", "id selector matches") ||
        !expect(value_of(button_style, "color") == "red", "class selector matches") ||
        !expect(value_of(button_style, "margin-left") == "8px", "second class token matches") ||
        !expect(value_of(tree.compute(sheet, label), "font-size") == "14px",
                "child resolves its own type selector")) {
        return 2;
    }

    // Non-matching class and id selectors must not leak onto the node.
    if (!expect(!button_style.find("color") || value_of(button_style, "color") != "blue",
                "absent class selector does not match") ||
        !expect(value_of(button_style, "border-top-width") != "1px",
                "absent id selector does not match")) {
        return 3;
    }

    // Pseudo-state is refreshed from the scene, so a compound selector re-evaluates.
    button.set_pseudo_state(PseudoState::Hover, true);
    if (!expect(tree.view(button)->state != PseudoState::None, "view refreshes pseudo-state") ||
        !expect(value_of(tree.compute(sheet, button), "color") == "green",
                "type.class:hover compound selector matches") ||
        // `color` inherits, so the child resolves the parent's hovered value through the cascade;
        // the compound selector itself must still not match the child.
        !expect(value_of(tree.compute(sheet, label), "color") == "green",
                "inherited color follows the hovered parent") ||
        !expect(value_of(tree.compute(sheet, label), "border-top-width") != "2px",
                "id selector still does not reach the child")) {
        return 4;
    }

    // A second tree over the same scene sees the same metadata, and classes do not dangle when many
    // nodes are built (the deque storage keeps references valid while entries are appended).
    {
        Node wide_root(10);
        wide_root.set_element_kind("View");
        auto wide_ptr = std::make_unique<Node>(11);
        Node& wide = wide_root.add_child(std::move(wide_ptr));
        wide.set_element_kind("Button");
        wide.set_class_name("a b c d e f g h i j k l");
        for (int index = 0; index < 16; ++index) {
            auto filler = std::make_unique<Node>(static_cast<tessera::ui::NodeId>(20 + index));
            filler->set_element_kind("Text");
            filler->set_class_name("filler");
            wide_root.add_child(std::move(filler));
        }
        SceneStyleTree second(wide_root);
        const auto* wide_view = second.view(wide);
        if (!expect(wide_view != nullptr && wide_view->classes.size() == 12,
                    "twelve class tokens") ||
            !expect(wide_view->classes.front() == "a" && wide_view->classes.back() == "l",
                    "tokens keep their storage after later inserts")) {
            return 5;
        }
        const auto wide_sheet = compile_stylesheet(R"(Button.l { color: red; })");
        if (!expect(value_of(second.compute(wide_sheet, wide), "color") == "red",
                    "last token still matches a class selector")) {
            return 5;
        }
    }

    std::cout << "scene_style_metadata: ok\n";
    return 0;
}
