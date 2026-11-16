// SPDX-License-Identifier: MIT

// One IR node -> one scene node (issue #166; the dynamic child forms are issue #389). This is the
// static half of the walk: creation, identity, metadata, focus/scroll flags and the text payload.
// Its children are `scene_build_children.cpp`'s job, because a child may be a conditional branch or
// a repeated child, and both of those need the state decisions `scene_build_dynamic.hpp` states.
//
// Every call here is one the generated C++ makes in the same order (`emit_node.cpp`'s numbered
// emission order), so a preview tree is the tree a native build would have built for the same IR.

#include "scene_build_internal.hpp"
#include "tessera/compiler/eval/number_text.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

// One decimal id field as the IR stores it (`uiId`/`automationId` are decimal strings, section 1 of
// docs/specs/automation-ids-v1.md). An empty field is "no id": `automationId` is optional and an
// unassigned `uiId` stays empty in an unsalted run.
bool parse_id(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

// The `Text` an IR node paints. A literal is used as-is; a template is evaluated, which is the
// point of running the evaluator at all: `Count: {count}` must print the state value, not the
// source.
bool evaluate_text(eval::Evaluator& evaluator, const ir::Expr& expr, const eval::Scope& scope,
                   std::string& out, std::string& error) {
    auto value = evaluator.evaluate(expr, scope);
    if (!value) {
        error = "text: " + value.error().code + ": " + value.error().message;
        return false;
    }
    if (const std::string* text = value->as_string()) {
        out = *text;
        return true;
    }
    // A non-string payload is printed the way the template printer prints it, so a `Text` whose
    // whole body is `{count}` reports "3" on both sides instead of failing.
    if (value->is_null()) {
        out.clear();
        return true;
    }
    if (const std::int32_t* whole = value->as_int()) {
        out = eval::int_text(*whole);
        return true;
    }
    if (const double* number = value->as_float()) {
        out = eval::float_text(*number);
        return true;
    }
    if (const bool* flag = value->as_bool()) {
        out = *flag ? "true" : "false";
        return true;
    }
    error = "text: a " + std::string(eval::type_name(*value)) + " has no printable form";
    return false;
}

ui::PaintData text_paint(std::string text) {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = std::move(text);
    return data;
}

// A literal string prop (`id`, `className`, `role`, `aria-*`), or nullptr when it is absent or
// dynamic. A dynamic value is codegen's dynamic-metadata path, which this slice does not evaluate.
const std::string* literal_string(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    if (found == node.props.end() || !found->second ||
        found->second->kind != ir::ExprKind::Literal) {
        return nullptr;
    }
    return std::get_if<std::string>(&found->second->literal);
}

// Applies one node's literal `style={{...}}` declarations. A dynamic value is not applied here: an
// animated or state-driven inline style is the frame pipeline's job (generated `apply_state`), and
// this slice lays out the component's initial tree.
void apply_inline_style(const ir::Node& node, ui::Node& scene) {
    for (const auto& [name, value] : node.style) {
        if (!value || value->kind != ir::ExprKind::Literal) {
            continue;
        }
        const auto& literal = value->literal;
        if (const auto* text = std::get_if<std::string>(&literal)) {
            scene.set_inline_style(name, *text);
        } else if (const auto* whole = std::get_if<std::int64_t>(&literal)) {
            scene.set_inline_style(name, std::to_string(*whole));
        } else if (const auto* number = std::get_if<double>(&literal)) {
            scene.set_inline_style(name, eval::float_text(*number));
        }
    }
}

} // namespace

bool Builder::build(const ir::Component& component) {
    // The window content node is a plain `View`, the shape `AppHost` gives a scene root; the
    // component's own root is its child, exactly as a generated base attaches its tree.
    root_.set_element_kind("View");
    return build_node(component.root, root_, 0);
}

bool Builder::build_node(const ir::Node& node, ui::Node& parent, std::size_t depth) {
    if (depth > kMaxPreviewDepth) {
        return fail("the tree nests deeper than the preview allows (" +
                    std::to_string(kMaxPreviewDepth) + ")");
    }
    if (++result_.nodes > kMaxPreviewNodes) {
        return fail("the tree has more nodes than the preview allows (" +
                    std::to_string(kMaxPreviewNodes) + ")");
    }
    switch (node.kind) {
    case ir::NodeKind::Component:
        return unsupported(
            "a user component instance (`" + node.id +
            "`) needs prop binding and slot splicing, which the preview does not evaluate yet");
    case ir::NodeKind::CustomElement:
        return unsupported("`CustomElement` nodes are not previewed yet");
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        return unsupported("a virtual collection (`" + node.id + "`) is not previewed yet");
    case ir::NodeKind::Slot:
        return unsupported("a `Slot` position (`" + node.id + "`) is not previewed yet");
    default:
        break;
    }

    auto created = std::make_unique<ui::Node>(next_node_id_++);
    ui::Node& scene = parent.add_child(std::move(created));
    scene.set_element_kind(ir::node_kind_name(node.kind));

    // Numeric identity: an explicit `automationId` replaces the computed `uiId` (section 2), and
    // the scene node's automation id is what the snapshot reports. Both are decimal strings.
    std::uint64_t id = 0;
    const bool has_id = parse_id(node.automation_id, id) || parse_id(node.ui_id, id);
    if (has_id) {
        scene.set_automation_id(id);
    }

    // The node's declared events, keyed by the automation id a dispatched scene event reports: that
    // id is how the session finds the handler to run, because `Node::bind_event` carries no IR
    // identity of its own. A node with a handler but no id cannot be addressed by a scripted click
    // (`click <automationId>`), so it is reported rather than silently unbound.
    if (!node.events.empty()) {
        if (!has_id) {
            return fail("a node with an event handler has no automation id (`" + node.id +
                        "`), so nothing can address it");
        }
        for (const auto& [event, handler] : node.events) {
            result_.handlers.push_back({id, event, handler});
        }
    }

    // Element metadata: the channels codegen fills (emit_node_attributes.cpp). They are how a
    // stylesheet selects the node and how the snapshot reports its accessible name.
    if (const std::string* text = literal_string(node, "id")) {
        scene.set_element_id(*text);
    }
    if (const std::string* text = literal_string(node, "className")) {
        scene.set_class_name(*text);
    }
    if (const std::string* text = literal_string(node, "role")) {
        scene.set_role(*text);
    }
    for (const auto& [name, value] : node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        if (const std::string* text = literal_string(node, name)) {
            (void)scene.set_aria(name, *text);
        }
    }
    apply_inline_style(node, scene);

    // A control is focusable and tab-reachable in generated code (emit_node_behavior.cpp), so the
    // preview must set it too: it decides the snapshot's `focused` state and how a control
    // participates in layout.
    if (node.kind == ir::NodeKind::Button || node.kind == ir::NodeKind::TextInput ||
        node.kind == ir::NodeKind::TextArea) {
        scene.set_focusable(true);
    }
    if (node.kind == ir::NodeKind::Scroll) {
        const std::string* axis = literal_string(node, "axis");
        const std::string_view value = axis != nullptr ? std::string_view(*axis) : "y";
        scene.set_scrollable_x(value == "x" || value == "both");
        scene.set_scrollable_y(value == "y" || value == "both");
    }

    if (node.text) {
        std::string text;
        if (!evaluate_text(evaluator_, *node.text, scope_, text, result_.error)) {
            return false;
        }
        scene.set_paint_data(text_paint(std::move(text)));
    } else if (node.kind == ir::NodeKind::Button) {
        // A `Button`'s caption is its `label` prop, not its text child, so without this a button
        // painted an empty box even once the preview had a font (issue #387).
        const auto label = node.props.find("label");
        if (label != node.props.end() && label->second != nullptr) {
            std::string text;
            if (!evaluate_text(evaluator_, *label->second, scope_, text, result_.error)) {
                return false;
            }
            scene.set_paint_data(text_paint(std::move(text)));
        }
    } else if (node.kind == ir::NodeKind::Text && !node.children.empty()) {
        // `<Text><b>x</b></Text>` is the inline-span form, which needs the span byte ranges codegen
        // computes while it creates the children (emit_node_behavior.cpp).
        return unsupported("an inline-span `Text` (`" + node.id + "`) is not previewed yet");
    }

    return build_children(node, scene, depth);
}

} // namespace tessera::compiler::codegen::cli::internal
