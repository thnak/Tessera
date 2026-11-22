// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event_type.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

// Element metadata of one scene node: what selector matching, codegen and the accessibility bridge
// read (kind, id, class, part, component, tag, role, automation id, item key), the bounded aria
// pair table, the inline style declarations, the generated event bindings, and the optional paint
// payload. Extracted from `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1, slice 2)
// so a change to one of those rules starts in its own header instead of the tree's most-included
// one.
//
// The type owns storage and no frame policy: a mutator returns the dirty domain its change implies
// (`Dirty::Clean` when nothing changed) and the owner applies it. That keeps "which domain does
// this rule invalidate" in one place and lets a metadata change compile without the geometry,
// scroll or interaction declarations.
//
// The storage layout is deliberately unchanged - plain views inline, each rare payload behind one
// lazily allocated pointer - because `kNodeSizeBudget` and the allocation-count test encode a
// measured decision for 10k-node screens (docs/design/performance.md, memory budgets).
namespace tessera::ui {

// Only ever named through a reference in a function-pointer signature and a dispatch call, so the
// payload header is not needed here. `event_type.hpp` above supplies `EventType`, which the binding
// table stores by value.
struct Event;

// Held behind a `unique_ptr`, so a forward declaration is enough for the member. The constructor
// and destructor are out of line for exactly this reason: their implicit versions instantiate
// `unique_ptr<optional<PaintData>>::~unique_ptr`, which needs the complete type.
struct PaintData;

// Element metadata is borrowed (docs/design/scene-graph.md section 5): every string is a view the
// caller keeps alive, so the caps below shorten a view and nothing here copies text into the node.
inline constexpr std::size_t kMaxMetadataBytes = 256;
inline constexpr std::size_t kMaxAriaPairs = 16;
inline constexpr std::size_t kMaxAriaNameBytes = 32;
inline constexpr std::size_t kMaxAriaValueBytes = 128;
inline constexpr std::size_t kMaxInlineStyleDeclarations = 16;
inline constexpr std::size_t kMaxInlineStyleNameBytes = 64;
inline constexpr std::size_t kMaxInlineStyleValueBytes = 256;
inline constexpr std::size_t kMaxEventBindings = 8;

struct AriaAttribute {
    std::string_view name; // the full attribute name, for example `aria-label`
    std::string_view value;

    bool operator==(const AriaAttribute&) const = default;
};

// Inline style declarations (compiler `style={{...}}`, docs/design/compiler.md section 11).
// Bounded and borrowed like the metadata views; the style tree applies them with Origin::Inline
// after the cascade.
struct InlineStyleDeclaration {
    std::string_view name;
    std::string_view value;

    bool operator==(const InlineStyleDeclaration&) const = default;
};

// Generated event bindings (docs/design/scene-graph.md section 6). Storage is bounded and
// allocation-free: at most kMaxEventBindings per node, invoked at the Target phase after the node's
// own on_event listeners and before bubbling. A binding receives a read-only const Event&, so it
// cannot consume the event.
using BoundEventHandler = void (*)(void* context, const Event& event);

struct EventBinding {
    EventType type = EventType::PointerDown;
    BoundEventHandler handler = nullptr;
    void* context = nullptr;
};

// Result of an inline-style write: whether the declaration was stored (false at the table cap or
// for an empty name) and which dirty domain the write implies. They are separate because rewriting
// the same declaration is accepted but changes nothing, so it must not dirty the frame.
struct StyleWrite {
    bool accepted = false;
    Dirty dirty = Dirty::Clean;

    bool operator==(const StyleWrite&) const = default;
};

class NodeMetadata {
public:
    // Out of line: the implicit versions instantiate
    // `unique_ptr<optional<PaintData>>::~unique_ptr`, which needs the complete `PaintData` - the
    // include this header deliberately avoids. See the forward-declaration note above.
    NodeMetadata();
    ~NodeMetadata();
    NodeMetadata(const NodeMetadata&) = delete;
    NodeMetadata& operator=(const NodeMetadata&) = delete;

    [[nodiscard]] std::string_view element_kind() const {
        return element_kind_;
    }
    // A node's kind is read by paint and by the text-selection gate but is not itself a selector
    // input (`element_tag` is), so setting it invalidates nothing.
    Dirty set_element_kind(std::string_view kind);

    [[nodiscard]] std::string_view element_id() const {
        return element_id_;
    }
    // `id` and `className` are selector inputs: setting or clearing them re-resolves this node.
    Dirty set_element_id(std::string_view id);
    Dirty clear_element_id();

    [[nodiscard]] std::string_view class_name() const {
        return class_name_;
    }
    Dirty set_class_name(std::string_view class_name);
    Dirty clear_class_name();

    // The `part` attribute (stylesheet-v1 section 6.3, ADR-047 item 3): the names that expose this
    // element to its component host's `::part(name)`.
    [[nodiscard]] std::string_view part() const {
        return component_metadata_ ? component_metadata_->part : std::string_view{};
    }
    Dirty set_part(std::string_view part);
    Dirty clear_part();

    // The component a component instance host was built from (`Button`); empty elsewhere. It types
    // the host for matching, so `::part()` can name it; the caller must mark the whole subtree,
    // because every descendant's rules are re-evaluated against it.
    [[nodiscard]] std::string_view component_name() const {
        return component_metadata_ ? component_metadata_->component_name : std::string_view{};
    }
    Dirty set_component_name(std::string_view component);
    Dirty clear_component_name();

    [[nodiscard]] std::string_view element_tag() const;
    Dirty set_element_tag(std::string_view tag);

    // The compiler's stable numeric identity; 0 means "not set".
    [[nodiscard]] std::uint64_t automation_id() const {
        return automation_id_;
    }
    Dirty set_automation_id(std::uint64_t automation_id);

    // List/grid instance identity (automation-ids-v1 section 1): a realized virtual item keeps its
    // template `automation_id` and carries the data-lifetime key here. Absent for plain nodes. The
    // "has a key" bit stays on `Node`'s flag word (`kHasItemKey`) rather than in this type: an
    // extra bool here would push the node from 304 to 320 bytes past `kNodeSizeBudget`, and key 0
    // is a legal key, so an in-band sentinel is not an option.
    [[nodiscard]] std::uint64_t item_key() const noexcept {
        return item_key_;
    }
    void set_item_key(std::uint64_t item_key) noexcept {
        item_key_ = item_key;
    }

    [[nodiscard]] std::string_view role() const {
        return role_;
    }
    Dirty set_role(std::string_view role);
    Dirty clear_role();

    // Stores or replaces one `aria-*` pair. False when the pair cap is reached or the name is
    // empty; the name is truncated to kMaxAriaNameBytes and the value to kMaxAriaValueBytes.
    bool set_aria(std::string_view name, std::string_view value);
    void clear_aria();
    // The stored pairs in insertion order: the accessor the accessibility bridge consumes.
    [[nodiscard]] std::span<const AriaAttribute> aria_attributes() const {
        return aria_ ? std::span<const AriaAttribute>(aria_->items.data(), aria_->count)
                     : std::span<const AriaAttribute>();
    }

    // Appends or replaces one declaration; `accepted` is false at the declaration cap or for an
    // empty name, and `dirty` is Clean when the pair was already stored verbatim.
    StyleWrite set_inline_style(std::string_view name, std::string_view value);
    // Drops one declaration; true when it existed. Declarations borrow their text, so an owner of
    // that text removes them before the text dies.
    bool remove_inline_style(std::string_view name);
    Dirty clear_inline_style();
    [[nodiscard]] std::span<const InlineStyleDeclaration> inline_style() const {
        return inline_style_ ? std::span<const InlineStyleDeclaration>(inline_style_->items.data(),
                                                                       inline_style_->count)
                             : std::span<const InlineStyleDeclaration>();
    }

    // False when `handler` is null or the node already has kMaxEventBindings bindings.
    bool bind_event(EventType type, BoundEventHandler handler, void* context = nullptr);
    // Removes the first binding for `type`; false when none was installed.
    bool unbind_event(EventType type);
    void clear_event_bindings();
    [[nodiscard]] bool has_event_binding(EventType type) const;
    [[nodiscard]] std::span<const EventBinding> event_bindings() const {
        return event_bindings_ ? std::span<const EventBinding>(event_bindings_->items.data(),
                                                               event_bindings_->count)
                               : std::span<const EventBinding>();
    }
    // Invokes every binding registered for `event.type` in registration order. Called by
    // dispatch_event_to; returns the number of bindings invoked.
    std::size_t run_event_bindings(const Event& event) const;

    // Optional app/compiler-supplied paint payload (D9): currently a text run painted after this
    // node's background and borders and inside the active clip. Purely visual - it never affects
    // layout, style, or hit testing. Text longer than PaintData::kMaxTextBytes is truncated at a
    // UTF-8 code-point boundary.
    // Defined in `node_metadata.cpp`: returning a reference to the empty slot needs the complete
    // `PaintData`, and this header must not include `paint_data.hpp`.
    [[nodiscard]] const std::optional<PaintData>& paint_data() const;
    Dirty set_paint_data(std::optional<PaintData> data);
    Dirty clear_paint_data();

private:
    // Rarely used payloads live behind lazily allocated pointers so a plain View pays one pointer
    // each instead of the full inline tables (docs/design/performance.md, memory budgets). A table
    // is allocated by the first set/bind and kept until the node dies; clearing only resets the
    // count, so a node that toggles a value never reallocates.
    struct AriaTable {
        std::array<AriaAttribute, kMaxAriaPairs> items{};
        std::size_t count = 0;
    };
    struct InlineStyleTable {
        std::array<InlineStyleDeclaration, kMaxInlineStyleDeclarations> items{};
        std::size_t count = 0;
    };
    struct BindingTable {
        std::array<EventBinding, kMaxEventBindings> items{};
        std::size_t count = 0;
    };
    // The component-library metadata (`part` names, the component name of a component instance host
    // and its element tag). All three are static strings of a component source and most nodes carry
    // none, so they live behind one lazily allocated record: a plain View pays the pointer only,
    // exactly like the other rare payloads above.
    struct ComponentMetadata {
        std::string_view part;
        std::string_view component_name;
        std::string_view element_tag;
    };

    // Borrowed views: the caller keeps the storage alive (codegen passes literals). No allocations.
    std::string_view element_kind_;
    std::string_view element_id_;
    std::string_view class_name_;
    std::string_view role_;
    std::uint64_t automation_id_ = 0;
    std::uint64_t item_key_ = 0;
    std::unique_ptr<AriaTable> aria_;
    std::unique_ptr<InlineStyleTable> inline_style_;
    std::unique_ptr<BindingTable> event_bindings_;
    std::unique_ptr<ComponentMetadata> component_metadata_;
    std::unique_ptr<std::optional<PaintData>> paint_data_;
};

} // namespace tessera::ui
