// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node_metadata.hpp"
#include "tessera/ui/scene/node_scroll.hpp"
#include "tessera/ui/style/cursor_value.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// One node of the retained scene graph: identity and tree structure, box geometry and transform,
// interaction and focus policy, dirty-domain bookkeeping, and hit testing.
//
// The two state-heavy halves live in their own headers and are composed here
// (docs/architecture/god-type-audit-v1.md section 3.1): `NodeScroll` owns the scroll runtime and
// `NodeMetadata` owns the element metadata, aria, inline style, event bindings and paint payload.
// `Node` keeps the public setters as the facade, because those calls carry frame policy (which
// dirty domain a change raises) that belongs to the tree and not to a value type. The storage is
// still inline in the node - one lazily allocated table per rare payload - so `kNodeSizeBudget` and
// the allocation-count test keep their meaning (docs/design/performance.md, memory budgets).
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
//
// They stay beside `Node`'s other flags rather than moving to `NodeScroll`: the three values and
// the two scrollable-axis bits pack into one byte, and that byte is what lets the ten scroll floats
// fit inside `kNodeSizeBudget` (docs/architecture/god-type-audit-v1.md section 3.1).
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
    // conditional branch or a keyed-list item at its source position without rebuilding siblings.
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
    [[nodiscard]] rhi::Rect subtree_paint_bounds() const; // cached; see node_geometry.cpp
    void invalidate_subtree_paint_bounds();
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
    // Layout's text-fit fact (issue #490): true when layout proved this node's text payload stays
    // inside the content box paint uses. `apply_layout` rewrites it every pass, so it is false
    // until one proved it; paint re-checks the payload before trusting it.
    void set_text_fits_box(bool fits);
    [[nodiscard]] bool text_fits_box() const {
        return flag(kTextFitsBox);
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
    // descendant cannot re-enable with `visibility: visible`), but the node keeps its layout box.
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

    // CSS `cursor` (stylesheet-v1 section 11, issue #191), copied from computed style by
    // `style::sync_interaction_style`. The scene stores the resolved enum, never the CSS text, so
    // hit testing, input and the platform seam read it without parsing. The property is inherited,
    // so the computed value already carries the ancestor resolution: a `pointer` parent with an
    // explicit `auto` child yields `Auto` on the child. The setter is a no-op when unchanged, so a
    // steady scene stays clean; it marks no dirty domain (no pixels change).
    void set_cursor(style::CursorKind value);
    [[nodiscard]] style::CursorKind cursor() const {
        return cursor_;
    }

    void set_focusable(bool focusable);
    [[nodiscard]] bool focusable() const {
        return flag(kFocusable);
    }

    // Sequential focus order key. The ordering contract lives on FocusManager::focus_next.
    void set_tab_index(int tab_index);
    [[nodiscard]] int tab_index() const {
        return tab_index_;
    }

    void set_auto_focus(bool auto_focus) {
        set_flag(kAutoFocus, auto_focus);
    }
    [[nodiscard]] bool auto_focus() const {
        return flag(kAutoFocus);
    }

    // JSX `focusTrap` and `focusGroup` (issue #236): a `focus_trap` node confines Tab traversal to
    // its subtree and restores the previous focus when it goes away; a `focus_group` node is a
    // roving-focus group whose focusable descendants form one Tab stop.
    enum class FocusGroup : std::uint8_t { None, Horizontal, Vertical, Both };
    void set_focus_trap(bool trap) {
        set_flag(kFocusTrap, trap);
    }
    [[nodiscard]] bool focus_trap() const {
        return flag(kFocusTrap);
    }
    void set_typeahead(bool typeahead) {
        set_flag(kTypeahead, typeahead);
    }
    [[nodiscard]] bool typeahead() const {
        return flag(kTypeahead);
    }
    void set_interactive_parent(bool interactive) {
        set_flag(kInteractiveParent, interactive);
    }
    [[nodiscard]] bool interactive_parent() const {
        return flag(kInteractiveParent);
    }
    void set_focus_group(FocusGroup group) {
        focus_group_ = group;
    }
    [[nodiscard]] FocusGroup focus_group() const {
        return focus_group_;
    }

    // Scroll state (docs/specs/scroll-and-gestures-v1.md section 1). The state is `NodeScroll`;
    // these forwarders keep the node's spelling and raise the damage each change implies. Toggling
    // a scrollable axis or the insets marks Paint|Render: the paint clip and hit-testing clipping
    // start or stop at this node even when the clamped offset stays the same.
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
        return scroll_.content_width();
    }
    [[nodiscard]] float content_height() const {
        return scroll_.content_height();
    }
    [[nodiscard]] float scroll_x() const {
        return scroll_.scroll_x();
    }
    [[nodiscard]] float scroll_y() const {
        return scroll_.scroll_y();
    }
    void set_scrollport_insets(float left, float top, float right, float bottom);
    [[nodiscard]] float scrollport_left() const {
        return scroll_.scrollport_left();
    }
    [[nodiscard]] float scrollport_top() const {
        return scroll_.scrollport_top();
    }
    [[nodiscard]] float scrollport_right() const {
        return scroll_.scrollport_right();
    }
    [[nodiscard]] float scrollport_bottom() const {
        return scroll_.scrollport_bottom();
    }
    [[nodiscard]] bool has_scrollport_insets() const {
        return scroll_.has_scrollport_insets();
    }
    // Clamps to [0, max(0, content - viewport)] per scrollable axis, marks Paint|Render when the
    // value changes (never Layout), and returns whether it changed.
    bool set_scroll_offset(float x, float y);
    // Content-to-world transform: world_transform() * translation(-scroll). Children live in
    // content space, so this maps their coordinates to the scene.
    [[nodiscard]] rhi::Mat3 content_transform() const;

    // Scroll runtime style inputs (stylesheet-v1 section 11): policy inputs for the controller, not
    // paint-visible on their own, so setting one marks no dirty domain. The first move the policy
    // causes (a chained wheel, a smooth-scroll tick) marks Paint|Render through set_scroll_offset.
    // The smooth-scroll target is a plain clamped offset so retargeting never jumps.
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
    void set_scroll_animation_target(float x, float y);
    void clear_scroll_animation();
    [[nodiscard]] bool scroll_animation_active() const {
        return flag(kScrollAnimationActive);
    }
    [[nodiscard]] float scroll_animation_target_x() const {
        return scroll_.animation_target_x();
    }
    [[nodiscard]] float scroll_animation_target_y() const {
        return scroll_.animation_target_y();
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

    // Element metadata, aria, inline style, event bindings and the paint payload. The rules and the
    // bounds live in `node_metadata.hpp`; these forwarders keep the node's vocabulary
    // (`Node::part`, `Node::set_aria`, `Node::AriaAttribute`), which generated code and the style,
    // accessibility and automation bridges spell, and they own the dirty domain each mutation
    // raises.
    using AriaAttribute = tessera::ui::AriaAttribute;
    using InlineStyleDeclaration = tessera::ui::InlineStyleDeclaration;
    using EventBinding = tessera::ui::EventBinding;
    using BoundEventHandler = tessera::ui::BoundEventHandler;

    static constexpr std::size_t kMaxMetadataBytes = tessera::ui::kMaxMetadataBytes;
    static constexpr std::size_t kMaxAriaPairs = tessera::ui::kMaxAriaPairs;
    static constexpr std::size_t kMaxAriaNameBytes = tessera::ui::kMaxAriaNameBytes;
    static constexpr std::size_t kMaxAriaValueBytes = tessera::ui::kMaxAriaValueBytes;
    static constexpr std::size_t kMaxInlineStyleDeclarations =
        tessera::ui::kMaxInlineStyleDeclarations;
    static constexpr std::size_t kMaxInlineStyleNameBytes = tessera::ui::kMaxInlineStyleNameBytes;
    static constexpr std::size_t kMaxInlineStyleValueBytes = tessera::ui::kMaxInlineStyleValueBytes;
    static constexpr std::size_t kMaxEventBindings = tessera::ui::kMaxEventBindings;

    void set_element_kind(std::string_view kind);
    [[nodiscard]] std::string_view element_kind() const {
        return metadata_.element_kind();
    }
    // `id` and `className` are selector inputs: setting or clearing them marks Dirty::Style.
    void set_element_id(std::string_view id);
    void clear_element_id();
    [[nodiscard]] std::string_view element_id() const {
        return metadata_.element_id();
    }
    void set_class_name(std::string_view class_name);
    void clear_class_name();
    [[nodiscard]] std::string_view class_name() const {
        return metadata_.class_name();
    }
    void set_part(std::string_view part);
    void clear_part();
    [[nodiscard]] std::string_view part() const {
        return metadata_.part();
    }
    // Setting or clearing a component name re-types the host, so it marks the whole subtree dirty.
    void set_component_name(std::string_view component);
    void clear_component_name();
    [[nodiscard]] std::string_view component_name() const {
        return metadata_.component_name();
    }
    void set_element_tag(std::string_view tag);
    [[nodiscard]] std::string_view element_tag() const {
        return metadata_.element_tag();
    }
    void set_automation_id(std::uint64_t automation_id);
    [[nodiscard]] std::uint64_t automation_id() const {
        return metadata_.automation_id();
    }
    // List/grid instance identity (automation-ids-v1 section 1): a realized virtual item keeps its
    // template `automation_id` and carries the data-lifetime key here. Absent for plain nodes. The
    // "has a key" bit is a node flag because 0 is a legal key.
    void set_item_key(std::uint64_t item_key) noexcept {
        metadata_.set_item_key(item_key);
        set_flag(kHasItemKey, true);
    }
    void clear_item_key() noexcept {
        metadata_.set_item_key(0);
        set_flag(kHasItemKey, false);
    }
    [[nodiscard]] std::optional<std::uint64_t> item_key() const noexcept {
        return flag(kHasItemKey) ? std::optional<std::uint64_t>(metadata_.item_key())
                                 : std::nullopt;
    }
    void set_role(std::string_view role);
    void clear_role();
    [[nodiscard]] std::string_view role() const {
        return metadata_.role();
    }
    // Stores or replaces one `aria-*` pair; false at the pair cap or for an empty name.
    bool set_aria(std::string_view name, std::string_view value) {
        return metadata_.set_aria(name, value);
    }
    void clear_aria() {
        metadata_.clear_aria();
    }
    [[nodiscard]] std::span<const AriaAttribute> aria_attributes() const {
        return metadata_.aria_attributes();
    }

    // Inline style declarations (compiler `style={{...}}`), applied with Origin::Inline after the
    // cascade. A write marks Dirty::Style unless it re-stated a stored pair verbatim.
    bool set_inline_style(std::string_view name, std::string_view value);
    void clear_inline_style();
    bool remove_inline_style(std::string_view name);
    [[nodiscard]] std::span<const InlineStyleDeclaration> inline_style() const {
        return metadata_.inline_style();
    }

    // Generated event bindings (docs/design/scene-graph.md section 6), invoked at the Target phase
    // after the node's own on_event listeners and before bubbling. Binding marks no dirty domain.
    bool bind_event(EventType type, BoundEventHandler handler, void* context = nullptr) {
        return metadata_.bind_event(type, handler, context);
    }
    bool unbind_event(EventType type) {
        return metadata_.unbind_event(type);
    }
    void clear_event_bindings() {
        metadata_.clear_event_bindings();
    }
    [[nodiscard]] bool has_event_binding(EventType type) const {
        return metadata_.has_event_binding(type);
    }
    [[nodiscard]] std::span<const EventBinding> event_bindings() const {
        return metadata_.event_bindings();
    }
    // Invokes every binding registered for `event.type` in registration order. Called by
    // dispatch_event_to; returns the number of bindings invoked.
    std::size_t run_event_bindings(const Event& event) const {
        return metadata_.run_event_bindings(event);
    }

    // Optional app/compiler-supplied paint payload (D9), purely visual: it marks Paint|Render only.
    void set_paint_data(std::optional<PaintData> data);
    void clear_paint_data();
    [[nodiscard]] const std::optional<PaintData>& paint_data() const {
        return metadata_.paint_data();
    }

    virtual void on_event(Event& event);

private:
    void refresh_subtree_dirty();

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
        kPointerEvents = 1u << 8,      // CSS `pointer-events: auto`
        kStyleHidden = 1u << 9,        // resolved `visibility: hidden|collapse`
        kTextFitsBox = 1u << 10,       // layout's text-fit fact (issue #490)
        kInteractiveParent = 1u << 11, // `useInteractiveRole` (issue #635); see the accessor
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
    // Cached subtree paint box (issue #735, slice C); `mutable` because paint fills it lazily
    // through a const reference. Width encodes the state: negative is "not computed", +infinity is
    // "computed and unbounded", else the box - so no separate validity flag is needed.
    mutable rhi::Rect subtree_paint_bounds_{0.0f, 0.0f, -1.0f, 0.0f};
    rhi::Mat3 transform_;
    int tab_index_ = 0;
    int z_index_ = 0;
    PaintOutsets paint_outsets_;
    float glass_blur_sigma_ = 0.0f;
    UserSelect user_select_ = UserSelect::Auto; // resolved `user-select` value
    // Resolved `cursor` value (issue #191). The initial `auto` is the platform default arrow.
    style::CursorKind cursor_ = style::CursorKind::Auto;
    FocusGroup focus_group_ = FocusGroup::None;
    // Packed scroll bookkeeping: the scrollable-axis flags and the scroll runtime style enums share
    // one byte. It sits here, not with the other flag words, because `NodeScroll`'s ten floats need
    // 4-byte alignment and this byte is exactly the one-byte hole between `focus_group_` and them -
    // measured with `offsetof`: the byte block ends at 152 either way, so putting it here costs
    // nothing and putting it after the floats costs 1 byte of padding plus 7 at the tail.
    //
    // Enum values are stored in declaration order; the masks are sized to the largest enumerator.
    // Defaults: not scrollable, `scroll-behavior: auto`, `overscroll-behavior: auto` and the
    // registry initial `overscroll-effect: platform` (ordinal 4).
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
    std::uint8_t scroll_bits_ = 0x4u << kOverscrollEffectShift;
    // The ten scroll floats follow the byte above, so they start at a 4-aligned offset with no
    // hole.
    NodeScroll scroll_;
    // The remaining scalar bytes total exactly 8 (4 + 2 + 1 + 1), which is `NodeMetadata`'s
    // alignment, so the rarely used metadata record starts on an 8-byte boundary with no tail
    // padding before it. Together with the byte above this is what keeps `sizeof(Node)` at 320:
    // every member is contiguous and the object ends exactly at its last byte.
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    std::uint16_t flags_ = kVisible | kPointerEvents;
    Dirty dirty_ = Dirty::Clean;
    Dirty subtree_dirty_ = Dirty::Clean;
    NodeMetadata metadata_;
};

// Memory budget of one scene node (docs/design/performance.md); raise it only with a recorded
// decision. History: 336 -> 320 (#285 flags word), 320 -> 312 (C4 member reorder), 312 -> 328
// (#735 slice C: the cached subtree paint box is one rhi::Rect with no padding left to reclaim).
// 328 -> 320: the `NodeScroll`/`NodeMetadata` split (god-type audit slice 2) re-derived the member
// order from `offsetof` instead of by eye, so the packed scroll byte fills the one-byte hole before
// the scroll floats and the 8-byte scalar tail lands exactly on `NodeMetadata`'s alignment. That
// removed all 8 bytes of padding, so the budget goes back down with no behavior change.
inline constexpr std::size_t kNodeSizeBudget = 320;
static_assert(sizeof(Node) <= kNodeSizeBudget, "ui::Node grew past kNodeSizeBudget");
// The split is the obvious place to leave 8 bytes of padding behind (a byte block immediately
// followed by an 8-aligned member), so on the 64-bit lanes the absence of padding is pinned as an
// exact size, which is a tighter claim than the budget. It is guarded on the pointer width because
// the number is a 64-bit layout fact: wasm32 has 4-byte pointers and therefore a smaller `Node`,
// where the budget assert above is the guard. MSVC measures 320 too, so both 64-bit lanes agree on
// this one number.
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(Node) == 320, "a padded Node is a regression even inside the budget");
#endif

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
