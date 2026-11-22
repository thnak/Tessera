// SPDX-License-Identifier: MIT
//
// `NodeMetadata` - element metadata, ARIA attributes, event bindings, inline styles and the paint
// payload (docs/architecture/god-type-audit-v1.md section 3.1, slice 2). The type owns storage and
// returns the dirty domain a mutation implies; the `Node` forwarders at the bottom of this file
// turn that answer into the node's dirty marks, so the frame policy stays on the tree and this TU
// never touches geometry or scroll state.

#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_metadata.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui {
namespace {

// Truncates to at most `max_bytes` without splitting a UTF-8 code point.
std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return std::string(text.substr(0, end));
}

// Element metadata is borrowed (docs/design/scene-graph.md section 5), so a cap only shortens the
// view; it never copies or allocates.
std::string_view bounded_view(std::string_view text, std::size_t max_bytes) {
    return text.substr(0, std::min(text.size(), max_bytes));
}

} // namespace

Dirty NodeMetadata::set_element_kind(std::string_view kind) {
    const std::string_view next = bounded_view(kind, kMaxMetadataBytes);
    if (next == element_kind_) {
        return Dirty::Clean;
    }
    element_kind_ = next;
    return Dirty::Clean;
}

Dirty NodeMetadata::set_element_id(std::string_view id) {
    const std::string_view next = bounded_view(id, kMaxMetadataBytes);
    if (next == element_id_) {
        return Dirty::Clean;
    }
    element_id_ = next;
    // `id` is a selector input: the stylesheet must re-resolve this node.
    return Dirty::Style;
}

Dirty NodeMetadata::clear_element_id() {
    if (element_id_.empty()) {
        return Dirty::Clean;
    }
    element_id_ = {};
    return Dirty::Style;
}

Dirty NodeMetadata::set_class_name(std::string_view class_name) {
    const std::string_view next = bounded_view(class_name, kMaxMetadataBytes);
    if (next == class_name_) {
        return Dirty::Clean;
    }
    class_name_ = next;
    return Dirty::Style;
}

Dirty NodeMetadata::clear_class_name() {
    if (class_name_.empty()) {
        return Dirty::Clean;
    }
    class_name_ = {};
    return Dirty::Style;
}

Dirty NodeMetadata::set_part(std::string_view part) {
    const std::string_view next = bounded_view(part, kMaxMetadataBytes);
    if (next.empty()) {
        return clear_part();
    }
    if (component_metadata_ && component_metadata_->part == next) {
        return Dirty::Clean;
    }
    if (!component_metadata_) {
        component_metadata_ = std::make_unique<ComponentMetadata>();
    }
    component_metadata_->part = next;
    // `part` is a selector input for the host's `::part()` rules.
    return Dirty::Style;
}

Dirty NodeMetadata::clear_part() {
    if (!component_metadata_ || component_metadata_->part.empty()) {
        return Dirty::Clean;
    }
    component_metadata_->part = {};
    return Dirty::Style;
}

std::string_view NodeMetadata::element_tag() const {
    return component_metadata_ ? component_metadata_->element_tag : std::string_view{};
}

Dirty NodeMetadata::set_element_tag(std::string_view tag) {
    const std::string_view next = bounded_view(tag, kMaxMetadataBytes);
    if (element_tag() == next) {
        return Dirty::Clean;
    }
    if (!component_metadata_) {
        component_metadata_ = std::make_unique<ComponentMetadata>();
    }
    component_metadata_->element_tag = next;
    return Dirty::Style;
}

Dirty NodeMetadata::set_component_name(std::string_view component) {
    const std::string_view next = bounded_view(component, kMaxMetadataBytes);
    if (next.empty()) {
        return clear_component_name();
    }
    if (component_metadata_ && component_metadata_->component_name == next) {
        return Dirty::Clean;
    }
    if (!component_metadata_) {
        component_metadata_ = std::make_unique<ComponentMetadata>();
    }
    component_metadata_->component_name = next;
    // Types the host for matching, so every `::part()` rule of the subtree re-resolves.
    return Dirty::Style;
}

Dirty NodeMetadata::clear_component_name() {
    if (!component_metadata_ || component_metadata_->component_name.empty()) {
        return Dirty::Clean;
    }
    component_metadata_->component_name = {};
    return Dirty::Style;
}

Dirty NodeMetadata::set_automation_id(std::uint64_t automation_id) {
    if (automation_id_ == automation_id) {
        return Dirty::Clean;
    }
    automation_id_ = automation_id;
    return Dirty::Clean;
}

Dirty NodeMetadata::set_role(std::string_view role) {
    const std::string_view next = bounded_view(role, kMaxMetadataBytes);
    if (next == role_) {
        return Dirty::Clean;
    }
    role_ = next;
    return Dirty::Clean;
}

Dirty NodeMetadata::clear_role() {
    role_ = {};
    return Dirty::Clean;
}

bool NodeMetadata::set_aria(std::string_view name, std::string_view value) {
    const std::string_view bounded_name = bounded_view(name, kMaxAriaNameBytes);
    if (bounded_name.empty()) {
        return false;
    }
    const std::string_view bounded_value = bounded_view(value, kMaxAriaValueBytes);
    if (!aria_) {
        aria_ = std::make_unique<AriaTable>();
    }
    for (std::size_t i = 0; i < aria_->count; ++i) {
        if (aria_->items[i].name == bounded_name) {
            aria_->items[i].value = bounded_value;
            return true;
        }
    }
    if (aria_->count == kMaxAriaPairs) {
        return false;
    }
    aria_->items[aria_->count++] = AriaAttribute{bounded_name, bounded_value};
    return true;
}

void NodeMetadata::clear_aria() {
    if (aria_) {
        aria_->count = 0;
    }
}

bool NodeMetadata::bind_event(EventType type, BoundEventHandler handler, void* context) {
    if (handler == nullptr) {
        return false;
    }
    if (!event_bindings_) {
        event_bindings_ = std::make_unique<BindingTable>();
    }
    if (event_bindings_->count == kMaxEventBindings) {
        return false;
    }
    event_bindings_->items[event_bindings_->count++] = EventBinding{type, handler, context};
    return true;
}

bool NodeMetadata::unbind_event(EventType type) {
    if (!event_bindings_) {
        return false;
    }
    BindingTable& table = *event_bindings_;
    for (std::size_t i = 0; i < table.count; ++i) {
        if (table.items[i].type != type) {
            continue;
        }
        for (std::size_t j = i; j + 1 < table.count; ++j) {
            table.items[j] = table.items[j + 1];
        }
        --table.count;
        table.items[table.count] = EventBinding{};
        return true;
    }
    return false;
}

void NodeMetadata::clear_event_bindings() {
    if (event_bindings_) {
        event_bindings_->count = 0;
    }
}

bool NodeMetadata::has_event_binding(EventType type) const {
    for (const EventBinding& binding : event_bindings()) {
        if (binding.type == type) {
            return true;
        }
    }
    return false;
}

std::size_t NodeMetadata::run_event_bindings(const Event& event) const {
    std::size_t invoked = 0;
    for (const EventBinding& binding : event_bindings()) {
        if (binding.type == event.type) {
            binding.handler(binding.context, event);
            ++invoked;
        }
    }
    return invoked;
}

StyleWrite NodeMetadata::set_inline_style(std::string_view name, std::string_view value) {
    const std::string_view bounded_name = bounded_view(name, kMaxInlineStyleNameBytes);
    if (bounded_name.empty()) {
        return {};
    }
    const std::string_view bounded_value = bounded_view(value, kMaxInlineStyleValueBytes);
    if (!inline_style_) {
        inline_style_ = std::make_unique<InlineStyleTable>();
    }
    InlineStyleTable& table = *inline_style_;
    for (std::size_t i = 0; i < table.count; ++i) {
        if (table.items[i].name != bounded_name) {
            continue;
        }
        if (table.items[i].value == bounded_value) {
            return {true, Dirty::Clean};
        }
        table.items[i].value = bounded_value;
        return {true, Dirty::Style};
    }
    if (table.count == kMaxInlineStyleDeclarations) {
        return {};
    }
    table.items[table.count++] = InlineStyleDeclaration{bounded_name, bounded_value};
    return {true, Dirty::Style};
}

bool NodeMetadata::remove_inline_style(std::string_view name) {
    if (!inline_style_) {
        return false;
    }
    InlineStyleTable& table = *inline_style_;
    for (std::size_t i = 0; i < table.count; ++i) {
        if (table.items[i].name != name) {
            continue;
        }
        for (std::size_t j = i + 1; j < table.count; ++j) {
            table.items[j - 1] = table.items[j];
        }
        --table.count;
        return true;
    }
    return false;
}

Dirty NodeMetadata::clear_inline_style() {
    if (!inline_style_ || inline_style_->count == 0) {
        return Dirty::Clean;
    }
    inline_style_->count = 0;
    return Dirty::Style;
}

NodeMetadata::NodeMetadata() = default;
NodeMetadata::~NodeMetadata() = default;

const std::optional<PaintData>& NodeMetadata::paint_data() const {
    static const std::optional<PaintData> kNone;
    return paint_data_ ? *paint_data_ : kNone;
}

Dirty NodeMetadata::set_paint_data(std::optional<PaintData> data) {
    if (data) {
        data->text = truncate_utf8(data->text, PaintData::kMaxTextBytes);
    }
    if (paint_data() == data) {
        return Dirty::Clean;
    }
    if (!data) {
        paint_data_.reset();
    } else if (paint_data_) {
        *paint_data_ = std::move(data);
    } else {
        paint_data_ = std::make_unique<std::optional<PaintData>>(std::move(data));
    }
    // Visual payload only: no style/layout invalidation.
    return Dirty::Paint | Dirty::Render;
}

Dirty NodeMetadata::clear_paint_data() {
    return set_paint_data(std::nullopt);
}

// --- Node forwarders ------------------------------------------------------------------------
//
// The public surface stays on `Node` so no call site moved with the split; each setter only turns
// the domain `NodeMetadata` reported into the node's dirty marks.

void Node::set_element_kind(std::string_view kind) {
    metadata_.set_element_kind(kind);
}

void Node::set_element_id(std::string_view id) {
    mark_dirty(metadata_.set_element_id(id));
}

void Node::clear_element_id() {
    mark_dirty(metadata_.clear_element_id());
}

void Node::set_class_name(std::string_view class_name) {
    mark_dirty(metadata_.set_class_name(class_name));
}

void Node::clear_class_name() {
    mark_dirty(metadata_.clear_class_name());
}

void Node::set_part(std::string_view part) {
    mark_dirty(metadata_.set_part(part));
}

void Node::clear_part() {
    mark_dirty(metadata_.clear_part());
}

void Node::set_element_tag(std::string_view tag) {
    mark_dirty(metadata_.set_element_tag(tag));
}

void Node::set_component_name(std::string_view component) {
    // Types the host for matching, so every `::part()` rule of the subtree re-resolves.
    const Dirty flags = metadata_.set_component_name(component);
    if (flags != Dirty::Clean) {
        mark_subtree_dirty(flags);
    }
}

void Node::clear_component_name() {
    const Dirty flags = metadata_.clear_component_name();
    if (flags != Dirty::Clean) {
        mark_subtree_dirty(flags);
    }
}

void Node::set_automation_id(std::uint64_t automation_id) {
    metadata_.set_automation_id(automation_id);
}

void Node::set_role(std::string_view role) {
    metadata_.set_role(role);
}

void Node::clear_role() {
    metadata_.clear_role();
}

bool Node::set_inline_style(std::string_view name, std::string_view value) {
    const StyleWrite write = metadata_.set_inline_style(name, value);
    mark_dirty(write.dirty);
    return write.accepted;
}

bool Node::remove_inline_style(std::string_view name) {
    if (!metadata_.remove_inline_style(name)) {
        return false;
    }
    mark_dirty(Dirty::Style);
    return true;
}

void Node::clear_inline_style() {
    mark_dirty(metadata_.clear_inline_style());
}

void Node::set_paint_data(std::optional<PaintData> data) {
    mark_dirty(metadata_.set_paint_data(std::move(data)));
}

void Node::clear_paint_data() {
    mark_dirty(metadata_.clear_paint_data());
}

} // namespace tessera::ui
