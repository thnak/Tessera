// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event_type.hpp"
#include "tessera/ui/scene/node_geometry_api.hpp"
#include "tessera/ui/scene/node_interaction_api.hpp"
#include "tessera/ui/scene/node_metadata.hpp"
#include "tessera/ui/scene/node_metadata_api.hpp"
#include "tessera/ui/scene/node_scroll.hpp"
#include "tessera/ui/scene/node_scroll_api.hpp"
#include "tessera/ui/scene/node_types.hpp"
#include "tessera/ui/style/cursor_value.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// One node of the retained scene graph: identity and tree structure, dirty-domain bookkeeping and
// hit testing, plus the storage of every role.
//
// The public surface is composed from four role bases (docs/architecture/god-type-audit-v1.md
// section 3.1), each in its own header so a change to one role's API starts there and not in the
// tree's most-included header:
//   `NodeGeometryApi`    box, transform, z-index, paint outsets, glass inputs, `world_*`;
//   `NodeInteractionApi` visibility, hit eligibility, focus policy, cursor, pseudo state;
//   `NodeScrollApi`      scrollable axes, offset, scrollport insets, scroll style inputs;
//   `NodeMetadataApi`    kind/id/class/part/role, aria, inline style, event bindings, paint
//   payload.
// The bases carry no data and no virtual member. The storage stays inline in the node - one lazily
// allocated table per rare payload - because `kNodeSizeBudget` and the allocation-count test encode
// a measured decision (docs/design/performance.md, memory budgets); the bases reach it through
// `node()` and are befriended below. `NodeScroll` and `NodeMetadata` own the two state-heavy
// halves. The setters stay on the node's own spelling because those calls carry frame policy (which
// dirty domain a change raises) that belongs to the tree and not to a value type.
namespace tessera::ui {

// `Event` is only ever named through a reference here (`on_event`, `run_event_bindings`,
// `dispatch_event`), so the payload header is not needed. `event_type.hpp` above supplies
// `EventType`, which the binding table stores by value. This is what keeps `node.hpp` - included by
// 80 translation units - from pulling the 41k-line `event.hpp`.
struct Event;

// MSVC applies the empty-base optimization to only the first of several empty bases unless the
// class opts in. The role bases are empty, so without this `sizeof(Node)` would grow by one padded
// word per extra base on the Windows lane.
#if defined(_MSC_VER) && !defined(__clang__)
#define TESSERA_EMPTY_BASES __declspec(empty_bases)
#else
#define TESSERA_EMPTY_BASES
#endif

class TESSERA_EMPTY_BASES Node : public NodeGeometryApi,
                                 public NodeInteractionApi,
                                 public NodeScrollApi,
                                 public NodeMetadataApi {
public:
    explicit Node(NodeId id = 0);
    // Out of line for the same reason `NodeMetadata`'s is: the implicit destructor instantiates
    // `unique_ptr<optional<PaintData>>::~unique_ptr`, which needs the complete `PaintData`.
    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Heap nodes start on a cache-line boundary. `sizeof(Node)` is five lines exactly, so an
    // aligned node occupies five lines and an unaligned one (16-byte malloc) straddles six, and
    // which fields share a line is only meaningful once the object's start is fixed. A class-level
    // allocator instead of `alignas(64)` on the type: an over-aligned `Node` would pad every
    // derived class (tests, generated components) and trip MSVC's C4324 on each. Stack and by-value
    // nodes are not aligned; that costs speed, never correctness.
    [[nodiscard]] static void* operator new(std::size_t size);
    static void operator delete(void* pointer, std::size_t size) noexcept;

    // The nested spellings generated code, tests and the bridges use; the types live in
    // `node_types.hpp` and `node_metadata.hpp`.
    using PaintOutsets = tessera::ui::PaintOutsets;
    using FocusGroup = tessera::ui::FocusGroup;
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

    void mark_dirty(Dirty flags);
    void clear_dirty(Dirty flags);
    // Marks `flags` on this node and every descendant (pre-order, allocation-free). Needed when an
    // ancestor's own state composites the whole subtree: a changed `opacity` fades every child, so
    // repainting only the ancestor's box would leave the children stale (paint-pass.md section
    // 14.1). The dirty pass then reports each node, which is what makes damage cover the subtree.
    void mark_subtree_dirty(Dirty flags);
    // Clears `flags` on this node and every descendant, and re-derives the summaries of the
    // ancestors once. The result is the same as calling `clear_dirty` on each dirty node, at one
    // pass over the dirty part of the subtree instead of a re-scan of every parent per cleared
    // child.
    void clear_subtree_dirty(Dirty flags);
    [[nodiscard]] Dirty dirty() const {
        return dirty_;
    }
    [[nodiscard]] Dirty subtree_dirty() const {
        return subtree_dirty_;
    }

    [[nodiscard]] bool hit_test(float x, float y) const;
    // Deepest hit target under the point, in paint order. A `pointer-events: none` node is not a
    // target but its children are still searched (stylesheet-v1 section 11). It stays a member of
    // `Node`: it is the mouse-move hot path and reads geometry, scroll and interaction state at
    // once.
    [[nodiscard]] Node* hit_test_deep(float x, float y);

    virtual void on_event(Event& event);

private:
    friend class NodeGeometryApi;
    friend class NodeInteractionApi;
    friend class NodeScrollApi;
    friend class NodeMetadataApi;
    friend struct NodeLayoutCheck; // pins the first-line layout in node.cpp

    void refresh_subtree_dirty();
    void clear_subtree_dirty_below(Dirty flags);

    // The two halves of `hit_test_deep` once the point has been mapped into a node's space, so the
    // descent maps it one level at a time instead of inverting the whole parent chain per node.
    // `hit_test_local` takes a point in this node's box space; `hit_test_in_parent_space` takes one
    // in the parent's content space, maps it into this node's space and prunes by the cached
    // subtree paint box before descending.
    [[nodiscard]] Node* hit_test_local(float local_x, float local_y);
    [[nodiscard]] Node* hit_test_in_parent_space(float parent_x, float parent_y);

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
        kHasZIndex = 1u << 12,    // `z_index_` is non-zero; lets a sibling walk skip reading it
        kHasTransform = 1u << 13, // `transform_` is not the identity
    };
    [[nodiscard]] bool flag(Flag bit) const noexcept {
        return (flags_ & bit) != 0;
    }
    void set_flag(Flag bit, bool on) noexcept {
        flags_ = static_cast<std::uint16_t>(on ? (flags_ | bit) : (flags_ & ~bit));
    }

    // --- Cache-line layout (docs/architecture/god-type-audit-v1.md section 3.1)
    // ---------------------
    //
    // `sizeof(Node)` is 320, five 64-byte lines, and heap nodes start on a line (`operator new`).
    // The first line holds everything the whole-scene passes read at every node - the dirty pass,
    // the hit-test descent and the paint cull walk - so a visit touches one line instead of four:
    // the vptr, the tree links, the box, the flag word, both dirty summaries and the packed bytes
    // the scroll and interaction checks read. The rest is reached only on a flag or when a pass
    // needs that field (`kHasZIndex`, `kHasTransform` stand in for the values below). Members are
    // placed by `offsetof`, with no padding anywhere; `NodeLayoutCheck` in node.cpp pins the first
    // line.
    //
    // line 0 (offsets 0..63): vptr, then
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    // `mutable` for the same reason as `subtree_paint_bounds_`: `set_span_bounds` writes it through
    // a const scene (issue #733 item 4). Every other write goes through `set_bounds`, which is
    // non-const, so the mutability is confined to the one paint-derived path.
    mutable rhi::Rect bounds_;
    std::uint16_t flags_ = kVisible | kPointerEvents;
    Dirty dirty_ = Dirty::Clean;
    Dirty subtree_dirty_ = Dirty::Clean;
    // Packed scroll bookkeeping: the scrollable-axis flags and the scroll runtime style enums share
    // one byte, read by every hit-test visit (`scrollable_x/y`). Enum values are stored in
    // declaration order; the masks are sized to the largest enumerator. Defaults: not scrollable,
    // `scroll-behavior: auto`, `overscroll-behavior: auto` and the registry initial
    // `overscroll-effect: platform` (ordinal 4).
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
    UserSelect user_select_ = UserSelect::Auto; // resolved `user-select` value
    // Resolved `cursor` value (issue #191). The initial `auto` is the platform default arrow.
    style::CursorKind cursor_ = style::CursorKind::Auto;
    FocusGroup focus_group_ = FocusGroup::None;

    // line 1 and on (offsets 64..): read on demand.
    //
    // Cached subtree paint box (issue #735, slice C); `mutable` because paint fills it lazily
    // through a const reference. Width encodes the state: negative is "not computed", +infinity is
    // "computed and unbounded", else the box - so no separate validity flag is needed.
    mutable rhi::Rect subtree_paint_bounds_{0.0f, 0.0f, -1.0f, 0.0f};
    rhi::Mat3 transform_; // meaningful only while `kHasTransform` is set; identity otherwise
    int z_index_ = 0;     // meaningful only while `kHasZIndex` is set; 0 otherwise
    int tab_index_ = 0;
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    PaintOutsets paint_outsets_;
    float glass_blur_sigma_ = 0.0f;
    // The ten scroll floats start at a 4-aligned offset with no hole.
    NodeScroll scroll_;
    // The last two members are the 8-aligned ones, so the object ends exactly at its last byte: an
    // 8-byte id, then the rarely used metadata record on an 8-byte boundary.
    NodeId id_ = 0;
    NodeMetadata metadata_;
};

// Memory budget of one scene node (docs/design/performance.md); raise it only with a recorded
// decision. History: 336 -> 320 (#285 flags word), 320 -> 312 (C4 member reorder), 312 -> 328
// (#735 slice C: the cached subtree paint box is one rhi::Rect with no padding left to reclaim).
// 328 -> 320: the `NodeScroll`/`NodeMetadata` split (god-type audit slice 2) re-derived the member
// order from `offsetof` instead of by eye, so the packed scroll byte fills the one-byte hole before
// the scroll floats and the 8-byte scalar tail lands exactly on `NodeMetadata`'s alignment. That
// removed all 8 bytes of padding, so the budget goes back down with no behavior change. The four
// role bases are empty and add nothing.
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

// The inline halves of the role bases need the complete `Node`, so they follow it.
#include "tessera/ui/scene/node_geometry_api_inl.hpp"
#include "tessera/ui/scene/node_interaction_api_inl.hpp"
#include "tessera/ui/scene/node_metadata_api_inl.hpp"
#include "tessera/ui/scene/node_scroll_api_inl.hpp"
