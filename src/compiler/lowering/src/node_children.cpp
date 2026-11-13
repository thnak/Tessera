// SPDX-License-Identifier: MIT

// Children of slices 7 and 8 (docs/specs/jsx-subset-v1.md sections 10.4 and 10.5, elements-basic.md
// section 0): fragments splice, text runs become the node `text`/`props.label` or synthetic `Text`
// children, conditional children contribute one or two conditional sibling nodes, and a `.map`
// child contributes the loop template node. The schema children rule of the element decides what
// is accepted.

#include "node_lowering_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

// --- inline spans (issue #229, elements-basic.md section 2.1, ir-v1 section 3.6) --------------

// Bounds of the paint payload that carries the run table (PaintData::spans).
constexpr std::size_t kMaxInlineSpans = 16;
constexpr std::size_t kMaxSpanTextBytes = 256;

// A Text element child is a span when it resolves to the Text intrinsic, including the lowercase
// aliases that map to it (span, b, strong, i, em, code, a).
bool is_span_tag(std::string_view tag) {
    if (tag == "Text") {
        return true;
    }
    const schema::AliasRow* alias = schema::find_alias(tag);
    return alias != nullptr && alias->status != schema::AliasStatus::Unsupported &&
           alias->target == "Text";
}

// A span may not change glyph advances or line boxes; the parent's measurement stays exact.
// `font-family` is accepted (the injected `code` default) but inert while the text stack paints one
// app face, so it cannot change metrics.
bool is_metric_style(std::string_view name) {
    return name == "font-size" || name == "letter-spacing" || name == "white-space";
}

bool is_span_prop(std::string_view name) {
    return name == "id" || name == "className" || name == "role" || name.rfind("aria-", 0) == 0;
}

std::size_t literal_size(const ir::Expr& expr) {
    if (expr.kind != ir::ExprKind::Literal) {
        return 0;
    }
    const auto* text = std::get_if<std::string>(&expr.literal);
    return text == nullptr ? 0 : text->size();
}

// Rejects an attribute/style/event a span does not have. The parent post-check keeps the span node
// a plain run: className/style/role/aria-*/id and onClick are the whole surface. A style default
// the alias injects (`code` -> font-family: monospace) is not an author override, so it is exempt
// from the metric rule; the runtime ignores it for measurement anyway.
bool check_span_node(NodeState& state, const ir::Node& span, std::string_view tag,
                     jsx::Span source) {
    if (span.key) {
        fail_node(state, "TESSERA_JSX_SPAN_ATTRIBUTE", "a span has no `key`", source);
        return false;
    }
    for (const auto& [name, value] : span.props) {
        (void)value;
        if (!is_span_prop(name)) {
            fail_node(state, "TESSERA_JSX_SPAN_ATTRIBUTE",
                      "`" + name + "` is not a span attribute; put it on the parent `Text`",
                      source);
            return false;
        }
    }
    for (const auto& [name, target] : span.events) {
        (void)target;
        if (name != "onClick") {
            fail_node(state, "TESSERA_JSX_SPAN_ATTRIBUTE",
                      "a span supports `onClick` only, not `" + name + "`", source);
            return false;
        }
    }
    const std::span<const schema::AliasStyle> injected = schema::alias_style_defaults(tag);
    for (const auto& [name, value] : span.style) {
        if (!is_metric_style(name)) {
            continue;
        }
        bool is_injected = false;
        for (const schema::AliasStyle& style : injected) {
            if (style.property == name && value && value->kind == ir::ExprKind::Literal &&
                std::get_if<std::string>(&value->literal) != nullptr &&
                *std::get_if<std::string>(&value->literal) == style.value) {
                is_injected = true;
                break;
            }
        }
        if (is_injected) {
            continue;
        }
        fail_node(state, "TESSERA_JSX_SPAN_METRIC",
                  "a span may not override `" + name +
                      "`: the parent measures and wraps the whole paragraph with its own metrics",
                  source, "use colour, font-weight, font-style or another non-metric style");
        return false;
    }
    return true;
}

// Text runs and `{expr}` children of a span: static text only in V1 (concatenation offsets must be
// known at build time). Nested elements are rejected as nested spans.
bool lower_span_text(NodeState& state, const std::vector<ChildItem>& items, ir::Node& node) {
    std::string text;
    for (const ChildItem& item : items) {
        if (!is_run_item(item)) {
            fail_node(state,
                      item.kind == ChildItem::Kind::Element ? "TESSERA_JSX_SPAN_NESTED"
                                                            : "TESSERA_JSX_CHILDREN_NOT_ALLOWED",
                      "a span contains text only; a nested element or a conditional/loop child is "
                      "not supported",
                      item.node->span);
            return false;
        }
        if (item.kind == ChildItem::Kind::Expression) {
            fail_node(state, "TESSERA_JSX_SPAN_DYNAMIC",
                      "inline spans require static text in V1; build the string in the component "
                      "or use separate `Text` nodes",
                      item.node->span);
            return false;
        }
        text += item.node->text;
    }
    node.text = ir::make_literal(std::move(text));
    return true;
}

// The span children of a `Text`: runs become synthetic spans, element children are lowered as
// spans with their own attributes, and the parent carries no text of its own.
bool lower_inline_span_children(NodeState& state, const std::vector<ChildItem>& items,
                                ir::Node& node) {
    std::string pending;
    std::vector<const ChildItem*> pending_items;
    std::size_t text_bytes = 0;
    const auto flush = [&]() -> bool {
        if (pending.empty()) {
            pending_items.clear();
            return true;
        }
        ir::Node span;
        span.kind = ir::NodeKind::Text;
        span.id = next_node_id(state);
        span.source =
            to_source_span(state.context.source_map, run_span(pending_items), state.context.file);
        span.text = ir::make_literal(std::move(pending));
        pending.clear();
        pending_items.clear();
        node.children.push_back(std::move(span));
        return true;
    };
    for (const ChildItem& item : items) {
        if (item.kind == ChildItem::Kind::Expression) {
            fail_node(state, "TESSERA_JSX_SPAN_DYNAMIC",
                      "inline spans require static text in V1; build the string in the component "
                      "or use separate `Text` nodes",
                      item.node->span);
            return false;
        }
        if (!is_run_item(item)) {
            if (item.kind != ChildItem::Kind::Element) {
                fail_node(state, "TESSERA_JSX_CHILDREN_NOT_ALLOWED",
                          "`<Text>` with inline spans accepts text and span elements only; a "
                          "conditional or loop child is not supported",
                          item.node->span);
                return false;
            }
            if (!is_span_tag(item.node->tag)) {
                fail_node(state, "TESSERA_JSX_CHILDREN_NOT_ALLOWED",
                          "`<" + std::string(item.node->tag) +
                              ">` is not an inline span; a `Text` accepts text and `Text` spans",
                          item.node->span,
                          "wrap the content in `<Text>` or use `span`/`b`/`strong`/`i`/`em`/`code`/"
                          "`a`");
                return false;
            }
            if (!flush()) {
                return false;
            }
            ++state.span_depth;
            std::optional<ir::Node> span = make_node(state, *item.node);
            --state.span_depth;
            if (!span) {
                return false;
            }
            if (!check_span_node(state, *span, item.node->tag, item.node->span)) {
                return false;
            }
            text_bytes += span->text ? literal_size(*span->text) : 0;
            node.children.push_back(std::move(*span));
            continue;
        }
        pending += item.node->text;
        text_bytes += item.node->text.size();
        pending_items.push_back(&item);
    }
    if (!flush()) {
        return false;
    }
    if (node.children.size() > kMaxInlineSpans) {
        fail_node(state, "TESSERA_JSX_SPAN_LIMIT", "a `Text` accepts at most 16 inline spans",
                  items.front().node->span);
        return false;
    }
    if (text_bytes > kMaxSpanTextBytes) {
        fail_node(state, "TESSERA_JSX_SPAN_LIMIT", "inline span text accepts at most 256 bytes",
                  items.front().node->span);
        return false;
    }
    return true;
}

bool lower_text_children(NodeState& state, const std::vector<ChildItem>& items, ir::Node& node) {
    if (state.span_depth > 0) {
        return lower_span_text(state, items, node);
    }
    const bool spans = std::any_of(items.begin(), items.end(), [](const ChildItem& item) {
        return item.kind == ChildItem::Kind::Element;
    });
    if (spans) {
        return lower_inline_span_children(state, items, node);
    }
    for (const ChildItem& item : items) {
        if (!is_run_item(item)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<Text>` accepts one text run; an element, conditional or loop child is "
                      "not supported (subset 10.4)",
                      item.node->span);
            return false;
        }
    }
    std::vector<const ChildItem*> run;
    run.reserve(items.size());
    for (const ChildItem& item : items) {
        run.push_back(&item);
    }
    jsx::Span span;
    ir::ExprPtr text = build_run(state, run, span);
    if (!text) {
        return false;
    }
    node.text = std::move(text);
    return true;
}

bool lower_button_children(NodeState& state, const std::vector<ChildItem>& items, ir::Node& node) {
    const bool has_element = std::any_of(items.begin(), items.end(),
                                         [](const ChildItem& item) { return !is_run_item(item); });
    if (has_element) {
        return append_runs_and_elements(state, items, node);
    }
    // Text-only children are the label (decision 11); an empty Button has neither, which the
    // analysis/schema constraints report.
    if (items.empty()) {
        return true;
    }
    std::vector<const ChildItem*> run;
    run.reserve(items.size());
    for (const ChildItem& item : items) {
        run.push_back(&item);
    }
    jsx::Span span;
    ir::ExprPtr label = build_run(state, run, span);
    if (!label) {
        return false;
    }
    node.props[std::string("label")] = std::move(label);
    return true;
}

// ir-v1 section 3.5 (elements-other.md section 3.1): a `VirtualList`/`VirtualGrid` takes exactly
// one child, a `.map` expression of the loop form; the callback element becomes the item template
// with its `loop` and `key`, and the collection has no children of its own.
bool lower_item_template(NodeState& state, const jsx::JsxNode& element, ir::Node& node) {
    const std::string tag = element.tag;
    std::vector<ChildItem> items;
    if (!flatten_children(state, element, items, true)) {
        return false;
    }
    if (items.empty()) {
        fail_node(state, "TESSERA_IR_VIRTUAL",
                  "`<" + tag + ">` needs one `.map` item template child", element.span,
                  "write `{rows.map((row) => <Row key={row.id} />)}`");
        return false;
    }
    if (items.size() > 1) {
        fail_node(state, "TESSERA_IR_VIRTUAL",
                  "`<" + tag + ">` takes exactly one `.map` item template child, found " +
                      std::to_string(items.size()) + " children",
                  items[1].node->span);
        return false;
    }
    const ChildItem& item = items.front();
    if (item.kind != ChildItem::Kind::Loop) {
        fail_node(state, "TESSERA_IR_VIRTUAL",
                  "the child of `<" + tag +
                      ">` must be a `.map` list expression, not text, an element or a conditional",
                  item.node->span);
        return false;
    }
    std::vector<ir::Node> produced;
    if (!lower_control_child(state, *item.expr, produced)) {
        return false;
    }
    if (produced.size() != 1) {
        fail_node(state, "TESSERA_IR_VIRTUAL", "an item template is exactly one node",
                  item.node->span);
        return false;
    }
    node.item_template = std::make_unique<ir::Node>(std::move(produced.front()));
    // virtualization-v1 section 4 and the section 13 register decision: an item whose real size is
    // not measured contributes the 32 px main-axis estimate. A collection that names neither
    // `itemSize` nor `estimateSize` therefore records the default estimate, so the runtime and the
    // IR snapshot agree on one deterministic number.
    if (node.props.find("itemSize") == node.props.end() &&
        node.props.find("estimateSize") == node.props.end()) {
        node.props[std::string("estimateSize")] = ir::make_literal(std::int64_t{32});
    }
    return true;
}

} // namespace

// Slice 9: the children of a component reference are the caller's static slot content. They follow
// the View rules (text runs become synthetic Text children) and are validated against the target
// component's `children` prop by slice 4, not here.
bool lower_slot_children(NodeState& state, const jsx::JsxNode& element, ir::Node& node) {
    std::vector<ChildItem> items;
    if (!flatten_children(state, element, items, false)) {
        return false;
    }
    // Slot content is the caller's nodes; the Slot itself belongs in the component whose prop
    // declares it (ir-v1 section 3.4).
    for (const ChildItem& item : items) {
        if (item.kind == ChildItem::Kind::Slot) {
            fail_node(state, "TESSERA_IR_SLOT",
                      "a Slot belongs in the component that declares `children`, not in slot "
                      "content",
                      item.node->span);
            return false;
        }
    }
    return append_runs_and_elements(state, items, node);
}

bool lower_children(NodeState& state, const jsx::JsxNode& element, const ElementInfo& info,
                    ir::Node& node) {
    const schema::ChildrenKind kind = info.row->children;
    if (kind == schema::ChildrenKind::Template) {
        return lower_item_template(state, element, node);
    }
    // ErrorBoundary requires one element child and rejects fragments (elements-other.md 4).
    const bool reject_fragments = kind == schema::ChildrenKind::One;
    std::vector<ChildItem> items;
    if (!flatten_children(state, element, items, reject_fragments)) {
        return false;
    }
    switch (kind) {
    case schema::ChildrenKind::None:
        if (!items.empty()) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` accepts no children",
                      items.front().node->span);
            return false;
        }
        return true;
    case schema::ChildrenKind::Text:
        return lower_text_children(state, items, node);
    case schema::ChildrenKind::InlineText:
        return lower_text_children(state, items, node);
    case schema::ChildrenKind::TextAndNodes:
        if (element.tag == "Button") {
            return lower_button_children(state, items, node);
        }
        return append_runs_and_elements(state, items, node);
    case schema::ChildrenKind::Nodes: {
        for (const ChildItem& item : items) {
            if (is_run_item(item)) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "`<" + std::string(element.tag) +
                              ">` accepts element children only; wrap text in `<Text>`",
                          item.node->span);
                return false;
            }
            if (!append_node_item(state, item, node)) {
                return false;
            }
        }
        return true;
    }
    case schema::ChildrenKind::One: {
        for (const ChildItem& item : items) {
            if (item.kind == ChildItem::Kind::Conditional || item.kind == ChildItem::Kind::Loop) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "`<" + std::string(element.tag) +
                              ">` does not accept conditional or loop children "
                              "(elements-other.md section 4)",
                          item.node->span);
                return false;
            }
        }
        if (items.size() != 1 || items.front().kind != ChildItem::Kind::Element) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` needs exactly one element child",
                      element.span);
            return false;
        }
        std::optional<ir::Node> child = make_node(state, *items.front().node);
        if (!child) {
            return false;
        }
        node.children.push_back(std::move(*child));
        return true;
    }
    case schema::ChildrenKind::Template:
        break;
    }
    return false;
}

} // namespace tessera::compiler::lowering::internal
