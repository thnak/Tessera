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

// Text-selection policy from `user-select` (stylesheet-v1 section 11, issue #247). `None` blocks
// starting a selection at this node, `Text` allows it, and `Auto` keeps the default (static text is
// selectable). The property is inherited, so each node stores its computed value and one explicit
// `auto`/`text` anywhere in a `none` subtree re-enables selection there.
enum class UserSelect : std::uint8_t {
    Auto,
    None,
    Text,
};

// Scroll runtime style inputs (docs/specs/stylesheet-v1.md section 11 and
// docs/specs/scroll-and-gestures-v1.md section 4). The values mirror the CSS keywords the registry
// accepts; the scene stores the resolved enum so the scroll controller and paint never parse CSS.
// `overscroll-effect` uses the names of docs/specs/animation-v1.md section 7 and has no `auto`
// keyword: `Platform` is the initial request the HAL profile resolves.
enum class ScrollBehavior : std::uint8_t {
    Auto,
    Smooth,
};
enum class OverscrollBehavior : std::uint8_t {
    Auto,
    Contain,
    None,
};
enum class OverscrollEffect : std::uint8_t {
    None,
    Glow,
    Stretch,
    Bounce,
    Platform,
};

class Node {
public:
    explicit Node(NodeId id = 0);
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    [[nodiscard]] NodeId id() const {
        return id_;
    }
    // Assigns the runtime id of a node that was not built with one (a pooled virtual item node,
    // which `NodeRecycler` constructs with id 0). The id participates in scene lookups (layout
    // boxes, keyed focus, input capture), so callers must assign a unique id before attaching the
    // node and must never change it afterwards.
    void set_id(NodeId id) {
        id_ = id;
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
    // Inline-span run rectangle, in the parent's border-box coordinates (issue #229). A span is not
    // a layout box: the paint pass derives this rect from the parent's line boxes and stores it so
    // the generic hit test can resolve a point to the span child. It never marks a dirty domain;
    // the parent's layout/paint dirty flags already drive the frame (a span has no independent
    // size).
    void set_span_bounds(const rhi::Rect& bounds);

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
        // True when an outer shadow has a blur radius: the one effect adaptive quality can reduce
        // (performance.md section 6.1). Sharp shadows reach outward too but have nothing to reduce.
        bool blurred = false;
    };
    void set_paint_outsets(const PaintOutsets& outsets);
    [[nodiscard]] const PaintOutsets& paint_outsets() const {
        return paint_outsets_;
    }

    // Glass (CSS `backdrop-filter`, docs/specs/glass-v1.md sections 2 and 7). The blur sigma in CSS
    // px is what paint::sync_scene_style resolved from the computed value, already clamped to the
    // configured `glassMaxBlurRadius`; 0 means the node has no non-`none` backdrop-filter. Damage
    // uses it to grow the node's paint dependency region by 3 sigma. `glass_subtree` is the derived
    // "this subtree contains a glass node" flag, so a frame whose tree has no glass node never
    // walks the scene looking for one. Both are paint-only: hit testing and layout ignore them.
    void set_glass_blur_sigma(float sigma);
    [[nodiscard]] float glass_blur_sigma() const {
        return glass_blur_sigma_;
    }
    void set_glass_subtree(bool has_glass);
    [[nodiscard]] bool glass_subtree() const {
        return flag(kGlassSubtree);
    }

    void set_visible(bool visible);
    [[nodiscard]] bool visible() const {
        return flag(kVisible);
    }

    // CSS `visibility: hidden|collapse` (stylesheet-v1 section 11, issue #213), copied from
    // computed style by `style::sync_interaction_style`. It is a separate flag from `visible()`
    // because the two have different owners: `visible()` is the programmatic/structural gate (a
    // conditional branch, a recycled virtual item), while this one is a stylesheet value that must
    // be able to flip back without resurrecting a node the program hid. V1 treats it like `display:
    // none` for painting, hit testing and the accessibility tree (the subtree is skipped, so a
    // descendant cannot re-enable with `visibility: visible`), but the node keeps its layout box,
    // which is the one part of the CSS behaviour that matters for a flex layout.
    void set_style_hidden(bool hidden);
    [[nodiscard]] bool style_hidden() const {
        return flag(kStyleHidden);
    }
    // True when this node takes part in painting, hit testing and the accessibility tree: both the
    // programmatic gate and the stylesheet value must allow it.
    [[nodiscard]] bool rendered() const {
        return flag(kVisible) && !flag(kStyleHidden);
    }

    // CSS `pointer-events: auto|none` (stylesheet-v1 section 11, issue #247), copied from computed
    // style by `style::sync_interaction_style`. False makes `hit_test_deep` pass this node over as
    // a target: hover, active, capture, click synthesis, wheel and accessibility-free hit tests
    // land on whatever is behind it. The subtree is still searched, so a descendant with `auto` is
    // hit again. Painting, keyboard focus and the accessibility tree are unaffected.
    void set_pointer_events(bool enabled);
    [[nodiscard]] bool pointer_events() const {
        return flag(kPointerEvents);
    }

    void set_user_select(UserSelect value);
    [[nodiscard]] UserSelect user_select() const {
        return user_select_;
    }
    // True when a selection gesture may start at this node: false only for an explicit/inherited
    // `user-select: none`. Editable text (`TextInput`/`TextArea`) always allows selection, whatever
    // an ancestor's `none` says, as in CSS.
    [[nodiscard]] bool text_selection_allowed() const;

    void set_focusable(bool focusable);
    [[nodiscard]] bool focusable() const {
        return flag(kFocusable);
    }

    // Sequential focus order key. The ordering contract lives on FocusManager::focus_next.
    void set_tab_index(int tab_index);
    [[nodiscard]] int tab_index() const {
        return tab_index_;
    }

    // JSX `autoFocus` (issue #226): the node asks to hold focus when its tree is first shown. It is
    // only a request; `FocusManager::apply_auto_focus` honours the first eligible node.
    void set_auto_focus(bool auto_focus) {
        set_flag(kAutoFocus, auto_focus);
    }
    [[nodiscard]] bool auto_focus() const {
        return flag(kAutoFocus);
    }

    // JSX `focusTrap` and `focusGroup` (issue #236). A visible node with `focus_trap` confines Tab
    // traversal to its subtree and restores the previous focus when it goes away
    // (`FocusManager::sync_focus_trap`). A node with a `focus_group` is a roving-focus group: its
    // focusable descendants form one Tab stop and the arrow keys of its axis move among them
    // (`FocusManager::move_in_focus_group`); the manager remembers which item holds the stop.
    enum class FocusGroup : std::uint8_t { None, Horizontal, Vertical, Both };
    void set_focus_trap(bool trap) {
        set_flag(kFocusTrap, trap);
    }
    [[nodiscard]] bool focus_trap() const {
        return flag(kFocusTrap);
    }
    // JSX `typeahead` on a focus group: typing characters moves focus to the next item whose
    // `aria-label` starts with them (`FocusManager::typeahead`).
    void set_typeahead(bool typeahead) {
        set_flag(kTypeahead, typeahead);
    }
    [[nodiscard]] bool typeahead() const {
        return flag(kTypeahead);
    }
    void set_focus_group(FocusGroup group) {
        focus_group_ = group;
    }
    [[nodiscard]] FocusGroup focus_group() const {
        return focus_group_;
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
        return (scroll_bits_ & kScrollableXBit) != 0;
    }
    [[nodiscard]] bool scrollable_y() const {
        return (scroll_bits_ & kScrollableYBit) != 0;
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

    // Scroll runtime style inputs (stylesheet-v1 section 11). They are policy inputs for the
    // controller, not paint-visible on their own: setting one marks no dirty domain, because the
    // first move the policy causes (a chained wheel, a smooth-scroll tick) marks Paint|Render
    // through set_scroll_offset. A style resolution pass fills them from computed style; layout is
    // never involved (`scrollbar-gutter` is the one scroll style property that is layout input, and
    // layout reads it from computed style directly).
    void set_scroll_behavior(ScrollBehavior behavior);
    [[nodiscard]] ScrollBehavior scroll_behavior() const {
        return (scroll_bits_ & kScrollBehaviorSmoothBit) != 0 ? ScrollBehavior::Smooth
                                                              : ScrollBehavior::Auto;
    }
    void set_overscroll_behavior(OverscrollBehavior behavior);
    [[nodiscard]] OverscrollBehavior overscroll_behavior() const {
        return static_cast<OverscrollBehavior>((scroll_bits_ & kOverscrollBehaviorMask) >>
                                               kOverscrollBehaviorShift);
    }
    void set_overscroll_effect(OverscrollEffect effect);
    [[nodiscard]] OverscrollEffect overscroll_effect() const {
        return static_cast<OverscrollEffect>((scroll_bits_ & kOverscrollEffectMask) >>
                                             kOverscrollEffectShift);
    }
    // Smooth programmatic scroll state (scroll-behavior: smooth). `scroll.cpp` owns the policy and
    // the fixed-step advance; the target is a plain clamped offset so retargeting never jumps.
    void set_scroll_animation_target(float x, float y);
    void clear_scroll_animation();
    [[nodiscard]] bool scroll_animation_active() const {
        return flag(kScrollAnimationActive);
    }
    [[nodiscard]] float scroll_animation_target_x() const {
        return scroll_animation_target_x_;
    }
    [[nodiscard]] float scroll_animation_target_y() const {
        return scroll_animation_target_y_;
    }

    // Interaction state seen by selector matching. Focus changes update the `Focus` bit and mark
    // the style domain dirty so the resolver re-evaluates `:focus`/`:not(:focus)`.
    void set_pseudo_state(style::PseudoState state);
    void set_pseudo_state(style::PseudoState state, bool enabled);
    [[nodiscard]] style::PseudoState pseudo_state() const {
        return pseudo_state_;
    }

    void mark_dirty(Dirty flags);
    void clear_dirty(Dirty flags);
    // Marks `flags` on this node and every descendant (pre-order, allocation-free). Needed when an
    // ancestor's own state composites the whole subtree: a changed `opacity` fades every child, so
    // repainting only the ancestor's box would leave the children stale (paint-pass.md section
    // 14.1). The dirty pass then reports each node, which is what makes damage cover the subtree.
    void mark_subtree_dirty(Dirty flags);
    [[nodiscard]] Dirty dirty() const {
        return dirty_;
    }
    [[nodiscard]] Dirty subtree_dirty() const {
        return subtree_dirty_;
    }

    [[nodiscard]] rhi::Mat3 world_transform() const;
    [[nodiscard]] rhi::Rect world_bounds() const;

    [[nodiscard]] bool hit_test(float x, float y) const;
    // Deepest hit target under the point, in paint order. A `pointer-events: none` node is not a
    // target but its children are still searched (stylesheet-v1 section 11).
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
    // The `part` attribute (stylesheet-v1 section 6.3, ADR-047 item 3): whitespace-separated names
    // that expose this element to its component host, so the host's stylesheet can target it with
    // `::part(name)`. Static metadata of the component source, like `className`: setting or
    // clearing it marks Dirty::Style and is a no-op when unchanged. Borrowed and bounded like every
    // other element metadata string.
    void set_part(std::string_view part);
    void clear_part();
    [[nodiscard]] std::string_view part() const {
        return component_metadata_ ? component_metadata_->part : std::string_view{};
    }
    // The component a component instance host was built from, for example `Button`. Empty on every
    // other node. It types the host for selector matching, so the host side of `::part()` can name
    // the component. Setting or clearing it changes matching for the whole subtree and marks
    // Dirty::Style on it.
    void set_component_name(std::string_view component);
    void clear_component_name();
    [[nodiscard]] std::string_view component_name() const {
        return component_metadata_ ? component_metadata_->component_name : std::string_view{};
    }
    void set_automation_id(std::uint64_t automation_id);
    [[nodiscard]] std::uint64_t automation_id() const {
        return automation_id_;
    }
    // List/grid instance identity (automation-ids-v1 section 1): a realized virtual item keeps its
    // template `automation_id` and carries the data-lifetime key here. Absent for plain nodes.
    void set_item_key(std::uint64_t item_key) noexcept {
        item_key_ = item_key;
        set_flag(kHasItemKey, true);
    }
    void clear_item_key() noexcept {
        item_key_ = 0;
        set_flag(kHasItemKey, false);
    }
    [[nodiscard]] std::optional<std::uint64_t> item_key() const noexcept {
        return flag(kHasItemKey) ? std::optional<std::uint64_t>(item_key_) : std::nullopt;
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
    // Drops one declaration; true when it existed. Declarations borrow their text, so an owner of
    // that text removes them before the text dies.
    bool remove_inline_style(std::string_view name);
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

    // The node's boolean state, one bit each. Ten separate bools cost more in padding than the
    // budget can spare (and MSVC's debug vector adds a pointer on top), so they share one word.
    // Defaults: visible, `pointer-events: auto`, everything else off.
    enum Flag : std::uint16_t {
        kVisible = 1u << 0,
        kFocusable = 1u << 1,
        kAutoFocus = 1u << 2,
        kFocusTrap = 1u << 3,
        kTypeahead = 1u << 4,
        kGlassSubtree = 1u << 5,
        kHasItemKey = 1u << 6,
        kScrollAnimationActive = 1u << 7,
        kPointerEvents = 1u << 8, // CSS `pointer-events: auto`
        kStyleHidden = 1u << 9,   // resolved `visibility: hidden|collapse`
    };
    [[nodiscard]] bool flag(Flag bit) const noexcept {
        return (flags_ & bit) != 0;
    }
    void set_flag(Flag bit, bool on) noexcept {
        flags_ = static_cast<std::uint16_t>(on ? (flags_ | bit) : (flags_ & ~bit));
    }

    NodeId id_ = 0;
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    rhi::Rect bounds_;
    rhi::Mat3 transform_;
    std::uint16_t flags_ = kVisible | kPointerEvents;
    FocusGroup focus_group_ = FocusGroup::None;
    int tab_index_ = 0;
    int z_index_ = 0;
    PaintOutsets paint_outsets_;
    float glass_blur_sigma_ = 0.0f;
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
    // The component-library metadata (`part` names and the component name of a component instance
    // host). Both are static strings of a component source and most nodes carry neither, so they
    // live behind one lazily allocated record: a plain View pays the pointer only, exactly like the
    // other rare payloads above.
    struct ComponentMetadata {
        std::string_view part;
        std::string_view component_name;
    };
    std::unique_ptr<std::optional<PaintData>> paint_data_;
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
    // Packed scroll bookkeeping: the two scrollable-axis flags and the three scroll runtime style
    // enums share one byte so the smooth-scroll target (two floats) fits inside kNodeSizeBudget.
    // Enum values are stored in declaration order; the masks are sized to the largest enumerator.
    static constexpr std::uint8_t kScrollableXBit = 1u << 0;
    static constexpr std::uint8_t kScrollableYBit = 1u << 1;
    static constexpr std::uint8_t kScrollBehaviorSmoothBit = 1u << 2;
    static constexpr std::uint8_t kOverscrollBehaviorShift = 3;
    static constexpr std::uint8_t kOverscrollBehaviorMask = 0x3u << kOverscrollBehaviorShift;
    static constexpr std::uint8_t kOverscrollEffectShift = 5;
    static constexpr std::uint8_t kOverscrollEffectMask = 0x7u << kOverscrollEffectShift;
    static_assert(static_cast<std::uint8_t>(OverscrollBehavior::None) == 2,
                  "OverscrollBehavior is packed in declaration order");
    static_assert(static_cast<std::uint8_t>(OverscrollEffect::Platform) == 4,
                  "OverscrollEffect is packed in declaration order");
    // Defaults: not scrollable, `scroll-behavior: auto`, `overscroll-behavior: auto` and the
    // registry initial `overscroll-effect: platform` (ordinal 4).
    std::uint8_t scroll_bits_ = 0x4u << kOverscrollEffectShift;
    std::unique_ptr<ComponentMetadata> component_metadata_;
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    float scrollport_left_ = 0.0f;
    float scrollport_top_ = 0.0f;
    float scrollport_right_ = 0.0f;
    float scrollport_bottom_ = 0.0f;
    float scroll_animation_target_x_ = 0.0f;
    float scroll_animation_target_y_ = 0.0f;
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    UserSelect user_select_ = UserSelect::Auto; // resolved `user-select` value
    Dirty dirty_ = Dirty::Clean;
    Dirty subtree_dirty_ = Dirty::Clean;
};

// Memory budget of one scene node (docs/design/performance.md, memory budgets): a 10k-node screen
// costs 10k times this plus Yoga and style storage. Raise it only with a recorded decision.
// 320 -> 336 (2026-11-14, issue #235): the component-parts record (`part` names and the component
// name of an instance host) added one pointer. 336 -> 320 (issue #285): the node's ten booleans now
// share one flags word, which took 24 bytes out (Release 312, MSVC Debug 320: its checked
// vector adds a pointer, so the Debug build is exactly at the budget). Recorded in performance.md.
inline constexpr std::size_t kNodeSizeBudget = 320;
static_assert(sizeof(Node) <= kNodeSizeBudget, "ui::Node grew past kNodeSizeBudget");

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
