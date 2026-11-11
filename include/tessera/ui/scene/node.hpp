// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::ui {

namespace rhi = tessera::rendering::rhi;

using NodeId = std::uint64_t;

class Node {
public:
    explicit Node(NodeId id = 0);
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    [[nodiscard]] NodeId id() const {
        return id_;
    }
    [[nodiscard]] Node* parent() const {
        return parent_;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const {
        return children_;
    }
    Node& add_child(std::unique_ptr<Node> child);
    // Inserts an owned child at `index` (clamped to the current child count, so a value at or above
    // size() appends). The dirty marks are the same as add_child. Generated code uses this to put a
    // conditional branch or a keyed-list item at its deterministic source position instead of
    // rebuilding the trailing siblings.
    Node& insert_child(std::size_t index, std::unique_ptr<Node> child);
    // Detaches and returns the owned child so callers can destroy or reattach a subtree. Returns
    // nullptr when `child` is not a direct child of this node.
    [[nodiscard]] std::unique_ptr<Node> remove_child(const Node& child);

    void set_bounds(const rhi::Rect& bounds);
    [[nodiscard]] const rhi::Rect& bounds() const {
        return bounds_;
    }

    void set_transform(const rhi::Mat3& transform);
    [[nodiscard]] const rhi::Mat3& transform() const {
        return transform_;
    }

    // Stacking order among siblings (CSS `z-index`, `auto` = 0): lower values paint first and hit
    // last; equal values keep child order. Visual only, so it dirties paint/render, not layout.
    void set_z_index(int z_index);
    [[nodiscard]] int z_index() const {
        return z_index_;
    }

    // How far this node paints beyond its border box (CSS box-shadow reach), in box-local px.
    // Damage uses it so a node's old and new pixels, including its shadow, are redrawn. Negative
    // and non-finite values count as 0.
    struct PaintOutsets {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };
    void set_paint_outsets(const PaintOutsets& outsets);
    [[nodiscard]] const PaintOutsets& paint_outsets() const {
        return paint_outsets_;
    }

    void set_visible(bool visible);
    [[nodiscard]] bool visible() const {
        return visible_;
    }

    void set_focusable(bool focusable);
    [[nodiscard]] bool focusable() const {
        return focusable_;
    }

    // Sequential focus order key. The ordering contract lives on FocusManager::focus_next.
    void set_tab_index(int tab_index);
    [[nodiscard]] int tab_index() const {
        return tab_index_;
    }

    // Scroll state (additive; docs/specs/scroll-and-gestures-v1.md section 1). The scrollport used
    // for clamping is the node's border box; `content_width/height` are supplied explicitly by
    // layout or the caller. Children are positioned in content space, so painting, hit testing, and
    // world transforms all apply -scroll to them while this node's own bounds/transform stay
    // unchanged. Toggling a scrollable axis marks Paint|Render: the paint clip and hit-testing
    // clipping start or stop at this node even when the clamped offset stays the same.
    void set_scrollable_x(bool scrollable);
    void set_scrollable_y(bool scrollable);
    [[nodiscard]] bool scrollable_x() const {
        return scrollable_x_;
    }
    [[nodiscard]] bool scrollable_y() const {
        return scrollable_y_;
    }
    void set_content_size(float width, float height);
    [[nodiscard]] float content_width() const {
        return content_width_;
    }
    [[nodiscard]] float content_height() const {
        return content_height_;
    }
    [[nodiscard]] float scroll_x() const {
        return scroll_x_;
    }
    [[nodiscard]] float scroll_y() const {
        return scroll_y_;
    }
    // Scrollport insets: how far the visible scrollport is inset from the border box on each side.
    // The clamp viewport is `bounds - insets`, hit testing clips the subtree to the inset box, and
    // the paint pass clips a scroll container to it when any inset is non-zero (otherwise it falls
    // back to the style-derived padding box). A caller that owns the computed style sets these to
    // the painted border widths so clamping, painting, and hit testing agree on a bordered
    // container. All zero by default (the border box), which keeps the previous behaviour
    // byte-identical. Negative/non-finite values sanitize to 0; each axis pair is clamped to the
    // box extent. A change marks Dirty::Paint | Dirty::Render and re-clamps the stored offset.
    void set_scrollport_insets(float left, float top, float right, float bottom);
    [[nodiscard]] float scrollport_left() const {
        return scrollport_left_;
    }
    [[nodiscard]] float scrollport_top() const {
        return scrollport_top_;
    }
    [[nodiscard]] float scrollport_right() const {
        return scrollport_right_;
    }
    [[nodiscard]] float scrollport_bottom() const {
        return scrollport_bottom_;
    }
    [[nodiscard]] bool has_scrollport_insets() const {
        return scrollport_left_ > 0.0f || scrollport_top_ > 0.0f || scrollport_right_ > 0.0f ||
               scrollport_bottom_ > 0.0f;
    }
    // Clamps to [0, max(0, content - viewport)] per scrollable axis, marks Dirty::Paint and
    // Dirty::Render when the value changes (never Layout), and returns whether it changed. A
    // non-scrollable axis clamps to 0 and never mutates.
    bool set_scroll_offset(float x, float y);
    // Content-to-world transform: world_transform() * translation(-scroll). Children live in
    // content space, so this maps their coordinates to the scene.
    [[nodiscard]] rhi::Mat3 content_transform() const;

    // Interaction state seen by selector matching. Focus changes update the `Focus` bit and mark
    // the style domain dirty so the resolver re-evaluates `:focus`/`:not(:focus)`.
    void set_pseudo_state(style::PseudoState state);
    void set_pseudo_state(style::PseudoState state, bool enabled);
    [[nodiscard]] style::PseudoState pseudo_state() const {
        return pseudo_state_;
    }

    void mark_dirty(Dirty flags);
    void clear_dirty(Dirty flags);
    [[nodiscard]] Dirty dirty() const {
        return dirty_;
    }
    [[nodiscard]] Dirty subtree_dirty() const {
        return subtree_dirty_;
    }

    [[nodiscard]] rhi::Mat3 world_transform() const;
    [[nodiscard]] rhi::Rect world_bounds() const;

    [[nodiscard]] bool hit_test(float x, float y) const;
    [[nodiscard]] Node* hit_test_deep(float x, float y);

    // Optional app/compiler-supplied paint payload (D9): currently a text run painted after this
    // node's background/borders and inside the active clip. Purely visual - it never affects
    // layout, style, or hit testing. Setting/clearing it marks Paint|Render only and is a no-op
    // when the payload is unchanged. Text longer than PaintData::kMaxTextBytes is truncated at a
    // UTF-8 code-point boundary.
    void set_paint_data(std::optional<PaintData> data);
    void clear_paint_data();
    [[nodiscard]] const std::optional<PaintData>& paint_data() const {
        static const std::optional<PaintData> kNone;
        return paint_data_ ? *paint_data_ : kNone;
    }

    // Element metadata (compiler codegen, docs/design/scene-graph.md section 5). Additive and
    // allocation-free: every string is a BORROWED view, so the caller must keep the storage alive
    // for as long as the node can be inspected (generated code passes string literals, whose
    // storage is static). Values are truncated to the documented byte caps rather than stored in a
    // node-owned buffer; the Node itself never copies metadata. `id`/`className` are the selector
    // inputs: setting or clearing them marks Dirty::Style and is a no-op when unchanged.
    // `automationId` is the compiler's stable numeric identity (slice 10 computes the final rule);
    // 0 means "not set".
    struct AriaAttribute {
        std::string_view name; // the full attribute name, for example `aria-label`
        std::string_view value;

        bool operator==(const AriaAttribute&) const = default;
    };

    static constexpr std::size_t kMaxMetadataBytes = 256;
    static constexpr std::size_t kMaxAriaPairs = 16;
    static constexpr std::size_t kMaxAriaNameBytes = 32;
    static constexpr std::size_t kMaxAriaValueBytes = 128;

    void set_element_kind(std::string_view kind);
    [[nodiscard]] std::string_view element_kind() const {
        return element_kind_;
    }
    void set_element_id(std::string_view id);
    void clear_element_id();
    [[nodiscard]] std::string_view element_id() const {
        return element_id_;
    }
    void set_class_name(std::string_view class_name);
    void clear_class_name();
    [[nodiscard]] std::string_view class_name() const {
        return class_name_;
    }
    void set_automation_id(std::uint64_t automation_id);
    [[nodiscard]] std::uint64_t automation_id() const {
        return automation_id_;
    }
    void set_role(std::string_view role);
    void clear_role();
    [[nodiscard]] std::string_view role() const {
        return role_;
    }
    // Stores or replaces one `aria-*` pair. False when the pair cap is reached or the name is
    // empty; the value is truncated to kMaxAriaValueBytes and the name to kMaxAriaNameBytes.
    bool set_aria(std::string_view name, std::string_view value);
    void clear_aria();
    // The stored pairs in insertion order: the accessor a future accessibility bridge consumes.
    [[nodiscard]] std::span<const AriaAttribute> aria_attributes() const {
        return aria_ ? std::span<const AriaAttribute>(aria_->items.data(), aria_->count)
                     : std::span<const AriaAttribute>();
    }

    // Inline style declarations (compiler `style={{...}}`, docs/design/compiler.md section 11).
    // Bounded and borrowed: the caller owns the strings (generated code passes literals or keeps
    // per-node members alive); the style tree applies them with Origin::Inline after the cascade.
    // Setting or clearing any declaration marks Dirty::Style and is a no-op when unchanged.
    struct InlineStyleDeclaration {
        std::string_view name;
        std::string_view value;

        bool operator==(const InlineStyleDeclaration&) const = default;
    };

    static constexpr std::size_t kMaxInlineStyleDeclarations = 16;
    static constexpr std::size_t kMaxInlineStyleNameBytes = 64;
    static constexpr std::size_t kMaxInlineStyleValueBytes = 256;

    // Appends/replaces one declaration; false when the declaration cap is reached or name empty.
    bool set_inline_style(std::string_view name, std::string_view value);
    void clear_inline_style();
    [[nodiscard]] std::span<const InlineStyleDeclaration> inline_style() const {
        return inline_style_ ? std::span<const InlineStyleDeclaration>(inline_style_->items.data(),
                                                                       inline_style_->count)
                             : std::span<const InlineStyleDeclaration>();
    }

    // Generated event bindings (docs/design/scene-graph.md section 6). Storage is bounded and
    // allocation-free: at most kMaxEventBindings per node, invoked at the Target phase after the
    // node's own on_event listeners and before bubbling. A binding receives a read-only const
    // Event&, so it cannot consume the event; binding or unbinding marks no dirty domain.
    using BoundEventHandler = void (*)(void* context, const Event& event);

    struct EventBinding {
        EventType type = EventType::PointerDown;
        BoundEventHandler handler = nullptr;
        void* context = nullptr;
    };

    static constexpr std::size_t kMaxEventBindings = 8;

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

    virtual void on_event(Event& event);

private:
    void refresh_subtree_dirty();
    void clamp_scrollport_insets();

    NodeId id_ = 0;
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    rhi::Rect bounds_;
    rhi::Mat3 transform_;
    bool visible_ = true;
    bool focusable_ = false;
    int tab_index_ = 0;
    int z_index_ = 0;
    PaintOutsets paint_outsets_;
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
    std::unique_ptr<std::optional<PaintData>> paint_data_;
    // Borrowed views: the caller keeps the storage alive (codegen passes literals). No allocations.
    std::string_view element_kind_;
    std::string_view element_id_;
    std::string_view class_name_;
    std::string_view role_;
    std::uint64_t automation_id_ = 0;
    std::unique_ptr<AriaTable> aria_;
    std::unique_ptr<InlineStyleTable> inline_style_;
    std::unique_ptr<BindingTable> event_bindings_;
    bool scrollable_x_ = false;
    bool scrollable_y_ = false;
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    float scrollport_left_ = 0.0f;
    float scrollport_top_ = 0.0f;
    float scrollport_right_ = 0.0f;
    float scrollport_bottom_ = 0.0f;
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    Dirty dirty_ = Dirty::Clean;
    Dirty subtree_dirty_ = Dirty::Clean;
};

// Memory budget of one scene node (docs/design/performance.md, memory budgets): a 10k-node screen
// costs 10k times this plus Yoga and style storage. Raise it only with a recorded decision.
inline constexpr std::size_t kNodeSizeBudget = 320;
static_assert(sizeof(Node) <= kNodeSizeBudget, "ui::Node grew past kNodeSizeBudget");

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
