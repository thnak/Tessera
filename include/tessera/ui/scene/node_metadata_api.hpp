// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/event_type.hpp"
#include "tessera/ui/scene/node_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Element-metadata surface of `ui::Node` (god-type audit section 3.1): kind, id, class, part,
// component, tag, role, automation id, item key, aria pairs, inline style, generated event bindings
// and the paint payload. The rules and the bounds live in `NodeMetadata` (`node_metadata.hpp`);
// these forwarders keep the node's vocabulary, which generated code and the style, accessibility
// and automation bridges spell, and they own the dirty domain each mutation raises.
//
// A data-free base of `Node`; see `NodeInteractionApi` for the layout reasoning. Bodies are in
// `node_metadata.cpp`, the pure forwards in `node_metadata_api_inl.hpp`.
namespace tessera::ui {

class Node;

class NodeMetadataApi {
public:
    void set_element_kind(std::string_view kind);
    [[nodiscard]] std::string_view element_kind() const;
    // `id` and `className` are selector inputs: setting or clearing them marks Dirty::Style.
    void set_element_id(std::string_view id);
    void clear_element_id();
    [[nodiscard]] std::string_view element_id() const;
    void set_class_name(std::string_view class_name);
    void clear_class_name();
    [[nodiscard]] std::string_view class_name() const;
    void set_part(std::string_view part);
    void clear_part();
    [[nodiscard]] std::string_view part() const;
    // Setting or clearing a component name re-types the host, so it marks the whole subtree dirty.
    void set_component_name(std::string_view component);
    void clear_component_name();
    [[nodiscard]] std::string_view component_name() const;
    void set_element_tag(std::string_view tag);
    [[nodiscard]] std::string_view element_tag() const;
    void set_automation_id(std::uint64_t automation_id);
    [[nodiscard]] std::uint64_t automation_id() const;
    // List/grid instance identity (automation-ids-v1 section 1): a realized virtual item keeps its
    // template `automation_id` and carries the data-lifetime key here. Absent for plain nodes. The
    // "has a key" bit is a node flag because 0 is a legal key.
    void set_item_key(std::uint64_t item_key) noexcept;
    void clear_item_key() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> item_key() const noexcept;
    void set_role(std::string_view role);
    void clear_role();
    [[nodiscard]] std::string_view role() const;
    // Stores or replaces one `aria-*` pair; false at the pair cap or for an empty name.
    bool set_aria(std::string_view name, std::string_view value);
    void clear_aria();
    [[nodiscard]] std::span<const AriaAttribute> aria_attributes() const;

    // Inline style declarations (compiler `style={{...}}`), applied with Origin::Inline after the
    // cascade. A write marks Dirty::Style unless it re-stated a stored pair verbatim.
    bool set_inline_style(std::string_view name, std::string_view value);
    void clear_inline_style();
    bool remove_inline_style(std::string_view name);
    [[nodiscard]] std::span<const InlineStyleDeclaration> inline_style() const;

    // Generated event bindings (docs/design/scene-graph.md section 6), invoked at the Target phase
    // after the node's own on_event listeners and before bubbling. Binding marks no dirty domain.
    bool bind_event(EventType type, BoundEventHandler handler, void* context = nullptr);
    bool unbind_event(EventType type);
    void clear_event_bindings();
    [[nodiscard]] bool has_event_binding(EventType type) const;
    [[nodiscard]] std::span<const EventBinding> event_bindings() const;
    // Invokes every binding registered for `event.type` in registration order. Called by
    // dispatch_event_to; returns the number of bindings invoked.
    std::size_t run_event_bindings(const Event& event) const;

    // Optional app/compiler-supplied paint payload (D9), purely visual: it marks Paint|Render only.
    void set_paint_data(std::optional<PaintData> data);
    void clear_paint_data();
    [[nodiscard]] const std::optional<PaintData>& paint_data() const;

protected:
    NodeMetadataApi() = default;
    ~NodeMetadataApi() = default;

private:
    [[nodiscard]] Node& node() noexcept;
    [[nodiscard]] const Node& node() const noexcept;
};

} // namespace tessera::ui
