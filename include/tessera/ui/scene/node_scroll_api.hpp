// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node_types.hpp"

// Scroll surface of `ui::Node` (docs/specs/scroll-and-gestures-v1.md section 1; god-type audit
// section 3.1). The state is `NodeScroll` plus one packed byte of axis bits and style enums, both
// stored inline in `Node` (the byte is what lets the ten scroll floats fit inside
// `kNodeSizeBudget`); this base adds the setters and the damage each change implies. Toggling a
// scrollable axis or the insets marks Paint|Render: the paint clip and hit-testing clipping start
// or stop at this node even when the clamped offset stays the same.
//
// A data-free base of `Node`; see `NodeInteractionApi` for the layout reasoning. Bodies are in
// `node_scroll.cpp`, the reads in `node_scroll_api_inl.hpp`.
namespace tessera::ui {

namespace rhi = tessera::rendering::rhi;

class Node;

class NodeScrollApi {
public:
    void set_scrollable_x(bool scrollable);
    void set_scrollable_y(bool scrollable);
    [[nodiscard]] bool scrollable_x() const;
    [[nodiscard]] bool scrollable_y() const;
    void set_content_size(float width, float height);
    [[nodiscard]] float content_width() const;
    [[nodiscard]] float content_height() const;
    [[nodiscard]] float scroll_x() const;
    [[nodiscard]] float scroll_y() const;
    void set_scrollport_insets(float left, float top, float right, float bottom);
    [[nodiscard]] float scrollport_left() const;
    [[nodiscard]] float scrollport_top() const;
    [[nodiscard]] float scrollport_right() const;
    [[nodiscard]] float scrollport_bottom() const;
    [[nodiscard]] bool has_scrollport_insets() const;
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
    [[nodiscard]] ScrollBehavior scroll_behavior() const;
    void set_overscroll_behavior(OverscrollBehavior behavior);
    [[nodiscard]] OverscrollBehavior overscroll_behavior() const;
    void set_overscroll_effect(OverscrollEffect effect);
    [[nodiscard]] OverscrollEffect overscroll_effect() const;
    void set_scroll_animation_target(float x, float y);
    void clear_scroll_animation();
    [[nodiscard]] bool scroll_animation_active() const;
    [[nodiscard]] float scroll_animation_target_x() const;
    [[nodiscard]] float scroll_animation_target_y() const;

protected:
    NodeScrollApi() = default;
    ~NodeScrollApi() = default;

private:
    [[nodiscard]] Node& node() noexcept;
    [[nodiscard]] const Node& node() const noexcept;
};

} // namespace tessera::ui
