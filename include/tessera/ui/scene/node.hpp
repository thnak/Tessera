// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cstdint>
#include <memory>
#include <optional>
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
        return paint_data_;
    }

    virtual void on_event(Event& event);

private:
    void refresh_subtree_dirty();

    NodeId id_ = 0;
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    rhi::Rect bounds_;
    rhi::Mat3 transform_;
    bool visible_ = true;
    bool focusable_ = false;
    int tab_index_ = 0;
    std::optional<PaintData> paint_data_;
    bool scrollable_x_ = false;
    bool scrollable_y_ = false;
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    Dirty dirty_ = Dirty::None;
    Dirty subtree_dirty_ = Dirty::None;
};

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
