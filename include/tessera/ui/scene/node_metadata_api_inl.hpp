// SPDX-License-Identifier: MIT

#pragma once

// Inline half of `NodeMetadataApi`. Included only by `node.hpp`, after `Node` is complete. Do not
// include this file directly.

namespace tessera::ui {

inline Node& NodeMetadataApi::node() noexcept {
    return static_cast<Node&>(*this);
}
inline const Node& NodeMetadataApi::node() const noexcept {
    return static_cast<const Node&>(*this);
}

inline std::string_view NodeMetadataApi::element_kind() const {
    return node().metadata_.element_kind();
}
inline std::string_view NodeMetadataApi::element_id() const {
    return node().metadata_.element_id();
}
inline std::string_view NodeMetadataApi::class_name() const {
    return node().metadata_.class_name();
}
inline std::string_view NodeMetadataApi::part() const {
    return node().metadata_.part();
}
inline std::string_view NodeMetadataApi::component_name() const {
    return node().metadata_.component_name();
}
inline std::string_view NodeMetadataApi::element_tag() const {
    return node().metadata_.element_tag();
}
inline std::uint64_t NodeMetadataApi::automation_id() const {
    return node().metadata_.automation_id();
}
inline void NodeMetadataApi::set_item_key(std::uint64_t item_key) noexcept {
    Node& self = node();
    self.metadata_.set_item_key(item_key);
    self.set_flag(Node::kHasItemKey, true);
}
inline void NodeMetadataApi::clear_item_key() noexcept {
    Node& self = node();
    self.metadata_.set_item_key(0);
    self.set_flag(Node::kHasItemKey, false);
}
inline std::optional<std::uint64_t> NodeMetadataApi::item_key() const noexcept {
    const Node& self = node();
    return self.flag(Node::kHasItemKey) ? std::optional<std::uint64_t>(self.metadata_.item_key())
                                        : std::nullopt;
}
inline std::string_view NodeMetadataApi::role() const {
    return node().metadata_.role();
}
inline bool NodeMetadataApi::set_aria(std::string_view name, std::string_view value) {
    return node().metadata_.set_aria(name, value);
}
inline void NodeMetadataApi::clear_aria() {
    node().metadata_.clear_aria();
}
inline std::span<const AriaAttribute> NodeMetadataApi::aria_attributes() const {
    return node().metadata_.aria_attributes();
}
inline std::span<const InlineStyleDeclaration> NodeMetadataApi::inline_style() const {
    return node().metadata_.inline_style();
}
inline bool NodeMetadataApi::bind_event(EventType type, BoundEventHandler handler, void* context) {
    return node().metadata_.bind_event(type, handler, context);
}
inline bool NodeMetadataApi::unbind_event(EventType type) {
    return node().metadata_.unbind_event(type);
}
inline void NodeMetadataApi::clear_event_bindings() {
    node().metadata_.clear_event_bindings();
}
inline bool NodeMetadataApi::has_event_binding(EventType type) const {
    return node().metadata_.has_event_binding(type);
}
inline std::span<const EventBinding> NodeMetadataApi::event_bindings() const {
    return node().metadata_.event_bindings();
}
inline std::size_t NodeMetadataApi::run_event_bindings(const Event& event) const {
    return node().metadata_.run_event_bindings(event);
}
inline const std::optional<PaintData>& NodeMetadataApi::paint_data() const {
    return node().metadata_.paint_data();
}

} // namespace tessera::ui
