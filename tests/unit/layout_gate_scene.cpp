// SPDX-License-Identifier: MIT
//
// The scene and edit machinery of the layout-unchanged gate's acceptance test (issue #735, slice
// A). See `layout_gate_scene.hpp` for why every box is a function of the stylesheet alone.

#include "layout_gate_scene.hpp"

namespace tessera::test::layout_gate {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;

namespace {

PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

} // namespace

void collect_boxes(const Node& node, Boxes& out) {
    const auto& bounds = node.bounds();
    out.push_back({node.id(), {bounds.x, bounds.y, bounds.width, bounds.height}});
    for (const auto& child : node.children()) {
        collect_boxes(*child, out);
    }
}

std::unique_ptr<Node> clone_tree(const Node& node) {
    auto copy = std::make_unique<Node>(node.id());
    copy->set_element_kind(node.element_kind());
    if (!node.element_id().empty()) {
        copy->set_element_id(node.element_id());
    }
    if (!node.class_name().empty()) {
        copy->set_class_name(node.class_name());
    }
    if (!node.part().empty()) {
        copy->set_part(node.part());
    }
    if (!node.component_name().empty()) {
        copy->set_component_name(node.component_name());
    }
    if (!node.element_tag().empty()) {
        copy->set_element_tag(node.element_tag());
    }
    if (!node.role().empty()) {
        copy->set_role(node.role());
    }
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        (void)copy->set_aria(aria.name, aria.value);
    }
    for (const Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        copy->set_inline_style(declaration.name, declaration.value);
    }
    if (node.paint_data().has_value()) {
        copy->set_paint_data(node.paint_data());
    }
    for (const auto& child : node.children()) {
        copy->add_child(clone_tree(*child));
    }
    return copy;
}

Scene::Scene(int card_count) {
    std::uint64_t id = 2;
    for (int i = 0; i < card_count; ++i) {
        Node& card = root->add_child(std::make_unique<Node>(id++));
        card.set_element_kind("View");
        card.set_class_name("card");
        cards.push_back(&card);
        for (int j = 0; j < 2; ++j) {
            Node& label = card.add_child(std::make_unique<Node>(id++));
            label.set_element_kind("Text");
            label.set_paint_data(text_payload("Item " + std::to_string(i)));
            labels.push_back(&label);
        }
    }
}

const char* name_of(Edit edit) {
    switch (edit) {
    case Edit::ClassPaint:
        return "class-paint";
    case Edit::ClassLayout:
        return "class-layout";
    case Edit::ClassDisplay:
        return "class-display";
    case Edit::InlinePaint:
        return "inline-paint";
    case Edit::InlineLayout:
        return "inline-layout";
    case Edit::Text:
        return "text";
    case Edit::FontSize:
        return "font-size";
    case Edit::Shape:
        return "shape";
    case Edit::Viewport:
        return "viewport";
    }
    return "?";
}

void apply_edit(Edit edit, Scene& scene, Lcg& rng, int frame, float& width, float& height) {
    const auto pick = [&rng, &scene](std::size_t first, std::size_t last) -> Node& {
        return *scene.cards[first + rng.below(static_cast<std::uint32_t>(last - first + 1))];
    };
    switch (edit) {
    case Edit::ClassPaint:
        // Only the paint-only token changes; the layout tokens are left exactly as they were.
        pick(0, 0).set_class_name((frame % 2) == 0 ? "card hot" : "card");
        break;
    case Edit::ClassLayout: {
        // Three distinct box-changing tokens, chosen by the serial so a repeat pick still changes
        // the class (and therefore the box) rather than being a no-op.
        const std::uint32_t which = (++scene.edit_serial) % 3;
        pick(1, 1).set_class_name(which == 0 ? "card wide"
                                             : (which == 1 ? "card tall" : "card padded"));
        break;
    }
    case Edit::ClassDisplay:
        pick(2, 2).set_class_name((frame % 2) == 0 ? "card hidden" : "card");
        break;
    case Edit::InlinePaint: {
        Node& card = pick(3, 3);
        card.set_inline_style("background-color", (frame % 2) == 0 ? "#ff0000" : "#00ff00");
        card.set_inline_style("border-radius", (frame % 2) == 0 ? "4px" : "0px");
        break;
    }
    case Edit::InlineLayout: {
        // Always a real change: the serial makes every write differ from the previous one, so this
        // case cannot degenerate into a no-op the gate would correctly accept. The text is owned by
        // the scene, because the node borrows it (`set_inline_style` keeps a `string_view`).
        Node& card = pick(4, 4);
        const std::uint32_t serial = ++scene.edit_serial;
        scene.inline_text.push_back(std::to_string(120 + (serial % 7)) + "px");
        card.set_inline_style("width", scene.inline_text.back());
        scene.inline_text.push_back(std::to_string(2 + (serial % 5)) + "px");
        card.set_inline_style("margin-top", scene.inline_text.back());
        break;
    }
    case Edit::Text: {
        // `set_paint_data` marks Paint|Render only: a text payload change is paint-only until the
        // caller says the new text must be re-measured. A real caller that changes text marks
        // Layout (see app_host_tests), so this case does the same - otherwise the warm host would
        // correctly skip layout while the cold reference measures the new text, and the two would
        // differ for a reason that has nothing to do with the gate.
        Node& label = *scene.labels[rng.below(static_cast<std::uint32_t>(scene.labels.size()))];
        label.set_paint_data(text_payload("Tick " + std::to_string(frame)));
        label.mark_dirty(tessera::ui::Dirty::Layout);
        break;
    }
    case Edit::FontSize: {
        Node& label = *scene.labels[rng.below(static_cast<std::uint32_t>(scene.labels.size()))];
        label.set_class_name((frame % 2) == 0 ? "big" : "");
        break;
    }
    case Edit::Shape: {
        // Add a child to a card, or remove the last one it has. Both move boxes. Only the extra
        // children this case adds are ever removed, so the `cards`/`labels` pointers the other
        // cases use stay valid.
        Node& host = pick(5, scene.cards.size() - 1);
        if (!host.children().empty() && host.children().back()->id() >= kExtraIdBase) {
            (void)host.remove_child(*host.children().back());
        } else {
            // A `View`, not a `Text`: `build_layout_input` skips a Text child of a Text parent
            // (inline spans are runs of the parent's text, not boxes), so a Text child added to a
            // card's Text label would never reach layout and the shape edit would be a no-op.
            auto extra =
                std::make_unique<Node>(kExtraIdBase + static_cast<std::uint64_t>(frame));
            extra->set_element_kind("View");
            extra->set_class_name("card");
            host.add_child(std::move(extra));
        }
        break;
    }
    case Edit::Viewport:
        width = (frame % 2) == 0 ? 640.0f : 800.0f;
        height = (frame % 2) == 0 ? 480.0f : 600.0f;
        break;
    }
}

bool moves_a_box(Edit edit) {
    switch (edit) {
    case Edit::ClassPaint:
    case Edit::InlinePaint:
        return false;
    case Edit::ClassLayout:
    case Edit::ClassDisplay:
    case Edit::InlineLayout:
    case Edit::Text:
    case Edit::FontSize:
    case Edit::Shape:
    case Edit::Viewport:
        return true;
    }
    return true;
}

} // namespace tessera::test::layout_gate
