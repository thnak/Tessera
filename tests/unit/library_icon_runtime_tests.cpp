// SPDX-License-Identifier: MIT

// Headless test of the library Icon (issue #530): the generated `IconDemo` draws a named icon, a
// small directional icon and a decorative icon inside a `Button`. It checks the semantics snapshot
// (a named icon exposes its name, a decorative icon is absent), the glyph (the right number of
// `currentColor` vector paths, a tint that follows the node color), the mirror flag (directional
// icons only) and that an icon inside a Button does not swallow the press. No GPU, no clock.

#include "IconDemo.hpp"
#include "a11y_test_helpers.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool has_class(const ui::Node& node, const std::string& name) {
    const std::string classes = " " + std::string(node.class_name()) + " ";
    return classes.find(" " + name + " ") != std::string::npos;
}

void collect(ui::Node& node, std::vector<ui::Node*>& vectors, std::vector<ui::Node*>& icons,
             std::vector<ui::Node*>& buttons) {
    const auto& data = node.paint_data();
    if (data.has_value() && data->kind == ui::PaintKind::Vector) {
        vectors.push_back(&node);
    }
    if (has_class(node, "ticon")) {
        icons.push_back(&node);
    }
    if (node.role() == "button") {
        buttons.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect(*child, vectors, icons, buttons);
    }
}

void size_all(ui::Node& node, float x, float y, float w, float h) {
    node.set_bounds({x, y, w, h});
    for (const auto& child : node.children()) {
        size_all(*child, 0.0f, 0.0f, w, h);
    }
}

ui::InputEvent pointer(ui::InputEventType type, float x, float y) {
    ui::InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = 1;
    event.pointer_kind = ui::PointerKind::Mouse;
    event.button = ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

} // namespace

int main() {
    ui::Node root{1000};
    icon_demo::IconDemo demo;
    demo.build(root);
    demo.apply_props();

    std::vector<ui::Node*> vectors;
    std::vector<ui::Node*> icons;
    std::vector<ui::Node*> buttons;
    collect(root, vectors, icons, buttons);

    check(icons.size() == 3, "three Icons are drawn (two loose, one in the Button)");
    check(buttons.size() == 1, "one Button");
    // home has two paths, chevron-right one, check one.
    check(vectors.size() == 4, "the glyphs draw four vector paths");
    for (ui::Node* path : vectors) {
        const ui::PaintData& data = *path->paint_data();
        check(data.vector.tint_current_color && !data.vector.tint.has_value(),
              "an icon path tints with the node color, with no per-icon color");
    }
    if (icons.size() != 3 || buttons.size() != 1) {
        return 1;
    }

    // Size classes and the mirror flag: only the directional icon carries the flip hook.
    check(has_class(*icons[0], "ticon-md") && has_class(*icons[0], "ticon-fixed"),
          "the named icon is md and not mirrored: " + std::string(icons[0]->class_name()));
    check(has_class(*icons[1], "ticon-sm") && has_class(*icons[1], "ticon-mirror"),
          "the directional icon is sm and mirrors: " + std::string(icons[1]->class_name()));
    check(has_class(*icons[2], "ticon-fixed") && !has_class(*icons[2], "ticon-mirror"),
          "an icon without the flag never mirrors");

    // Semantics snapshot: the named icon is an image with its name, decorative icons are absent.
    size_all(root, 0.0f, 0.0f, 20.0f, 20.0f);
    a11y_test::StubStyles styles;
    a11y_test::A11ySnapshot snapshot;
    a11y_test::build(root, styles, snapshot);
    std::size_t images = 0;
    std::size_t presentation_boxes = 0;
    bool named_home = false;
    for (const a11y_test::A11yNode& node : snapshot.nodes) {
        if (node.role == a11y_test::Role::Image) {
            ++images;
            named_home = named_home || a11y_test::name_of(node) == "Home";
        }
        if (node.role == a11y_test::Role::Presentation) {
            ++presentation_boxes;
        }
    }
    check(images == 1 && named_home, "exactly one image, named Home");
    check(presentation_boxes == 0, "decorative icons are not in the tree");

    // A press on the icon inside the Button reaches the caller's onPress.
    ui::Node& button = *buttons[0];
    if (ui::Node* host = button.parent()) {
        host->set_bounds({10, 100, 60, 32});
    }
    size_all(button, 0.0f, 0.0f, 60.0f, 32.0f);
    // The icon fills the Button's first 20 px; press at its center.
    ui::InputRouter router;
    router.dispatch(root, pointer(ui::InputEventType::PointerDown, 20, 110));
    router.dispatch(root, pointer(ui::InputEventType::PointerUp, 20, 110));
    check(demo.count() == 1, "a click on the icon inside a Button presses the Button");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "library_icon_runtime: ok\n";
    return 0;
}
