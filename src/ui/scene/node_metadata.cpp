// SPDX-License-Identifier: MIT
//
// Node element metadata, ARIA attributes, event bindings, inline styles and paint payload. Split
// from node.cpp (structure, bounds, scroll, dirty flags, hit testing) to respect the file limits.

#include "tessera/ui/scene/node.hpp"

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

void Node::set_element_kind(std::string_view kind) {
    const std::string_view next = bounded_view(kind, kMaxMetadataBytes);
    if (next == element_kind_) {
        return;
    }
    element_kind_ = next;
}

void Node::set_element_id(std::string_view id) {
    const std::string_view next = bounded_view(id, kMaxMetadataBytes);
    if (next == element_id_) {
        return;
    }
    element_id_ = next;
    // `id` is a selector input: the stylesheet must re-resolve this node.
    mark_dirty(Dirty::Style);
}

void Node::clear_element_id() {
    if (element_id_.empty()) {
        return;
    }
    element_id_ = {};
    mark_dirty(Dirty::Style);
}

void Node::set_class_name(std::string_view class_name) {
    const std::string_view next = bounded_view(class_name, kMaxMetadataBytes);
    if (next == class_name_) {
        return;
    }
    class_name_ = next;
    mark_dirty(Dirty::Style);
}

void Node::clear_class_name() {
    if (class_name_.empty()) {
        return;
    }
    class_name_ = {};
    mark_dirty(Dirty::Style);
}

void Node::set_automation_id(std::uint64_t automation_id) {
    if (automation_id_ == automation_id) {
        return;
    }
    automation_id_ = automation_id;
}

void Node::set_role(std::string_view role) {
    const std::string_view next = bounded_view(role, kMaxMetadataBytes);
    if (next == role_) {
        return;
    }
    role_ = next;
}

void Node::clear_role() {
    role_ = {};
}

bool Node::set_aria(std::string_view name, std::string_view value) {
    const std::string_view bounded_name = bounded_view(name, kMaxAriaNameBytes);
    if (bounded_name.empty()) {
        return false;
    }
    const std::string_view bounded_value = bounded_view(value, kMaxAriaValueBytes);
    for (std::size_t i = 0; i < aria_count_; ++i) {
        if (aria_[i].name == bounded_name) {
            aria_[i].value = bounded_value;
            return true;
        }
    }
    if (aria_count_ == kMaxAriaPairs) {
        return false;
    }
    aria_[aria_count_++] = AriaAttribute{bounded_name, bounded_value};
    return true;
}

void Node::clear_aria() {
    aria_count_ = 0;
}

bool Node::bind_event(EventType type, BoundEventHandler handler, void* context) {
    if (handler == nullptr || event_binding_count_ == kMaxEventBindings) {
        return false;
    }
    event_bindings_[event_binding_count_++] = EventBinding{type, handler, context};
    return true;
}

bool Node::unbind_event(EventType type) {
    for (std::size_t i = 0; i < event_binding_count_; ++i) {
        if (event_bindings_[i].type != type) {
            continue;
        }
        for (std::size_t j = i; j + 1 < event_binding_count_; ++j) {
            event_bindings_[j] = event_bindings_[j + 1];
        }
        --event_binding_count_;
        event_bindings_[event_binding_count_] = EventBinding{};
        return true;
    }
    return false;
}

void Node::clear_event_bindings() {
    event_binding_count_ = 0;
}

bool Node::has_event_binding(EventType type) const {
    for (std::size_t i = 0; i < event_binding_count_; ++i) {
        if (event_bindings_[i].type == type) {
            return true;
        }
    }
    return false;
}

std::size_t Node::run_event_bindings(const Event& event) const {
    std::size_t invoked = 0;
    for (std::size_t i = 0; i < event_binding_count_; ++i) {
        const EventBinding& binding = event_bindings_[i];
        if (binding.type == event.type) {
            binding.handler(binding.context, event);
            ++invoked;
        }
    }
    return invoked;
}

bool Node::set_inline_style(std::string_view name, std::string_view value) {
    const std::string_view bounded_name = bounded_view(name, kMaxInlineStyleNameBytes);
    if (bounded_name.empty()) {
        return false;
    }
    const std::string_view bounded_value = bounded_view(value, kMaxInlineStyleValueBytes);
    for (std::size_t i = 0; i < inline_style_count_; ++i) {
        if (inline_style_[i].name != bounded_name) {
            continue;
        }
        if (inline_style_[i].value == bounded_value) {
            return true;
        }
        inline_style_[i].value = bounded_value;
        mark_dirty(Dirty::Style);
        return true;
    }
    if (inline_style_count_ == kMaxInlineStyleDeclarations) {
        return false;
    }
    inline_style_[inline_style_count_++] = InlineStyleDeclaration{bounded_name, bounded_value};
    mark_dirty(Dirty::Style);
    return true;
}

void Node::clear_inline_style() {
    if (inline_style_count_ == 0) {
        return;
    }
    inline_style_count_ = 0;
    mark_dirty(Dirty::Style);
}

void Node::set_paint_data(std::optional<PaintData> data) {
    if (data) {
        data->text = truncate_utf8(data->text, PaintData::kMaxTextBytes);
    }
    if (paint_data_ == data) {
        return;
    }
    paint_data_ = std::move(data);
    // Visual payload only: no style/layout invalidation.
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::clear_paint_data() {
    set_paint_data(std::nullopt);
}

} // namespace tessera::ui
