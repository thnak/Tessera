// SPDX-License-Identifier: MIT

#pragma once

// Overlay placement (docs/specs/overlays-and-layers-v1.md section 3, issue #302): where a box of a
// given size goes relative to an anchor, flipped and shifted to stay inside the safe viewport. A
// pure function over plain rectangles: no scene, no allocation.

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>

namespace tessera::ui {

// Which side of the anchor the overlay takes. `Start` and `End` are logical (swapped under RTL).
// For a window anchor the side is the window edge the overlay sits against, inside the window.
enum class OverlaySide : std::uint8_t { Top, Bottom, Start, End, Center };

// Alignment along the cross axis: the anchor's start edge, its centre, or its end edge. For a
// top or bottom side start is the left edge (right under RTL); for a start or end side it is the
// top edge.
enum class OverlayAlign : std::uint8_t { Start, Center, End };

struct OverlayAnchor {
    enum class Kind : std::uint8_t { Window, Rect, Node };

    Kind kind = Kind::Window;
    rendering::rhi::Rect rect; // Kind::Rect, in window logical pixels
    NodeId node = 0;           // Kind::Node: a scene node, tracked through layout and scroll

    [[nodiscard]] static OverlayAnchor window() {
        return {};
    }
    [[nodiscard]] static OverlayAnchor at(const rendering::rhi::Rect& rect) {
        OverlayAnchor anchor;
        anchor.kind = Kind::Rect;
        anchor.rect = rect;
        return anchor;
    }
    [[nodiscard]] static OverlayAnchor of(NodeId node) {
        OverlayAnchor anchor;
        anchor.kind = Kind::Node;
        anchor.node = node;
        return anchor;
    }
};

struct OverlayPlacement {
    OverlayAnchor anchor;
    OverlaySide side = OverlaySide::Bottom;
    OverlayAlign align = OverlayAlign::Center;
    // Distance from the anchor (or inward from the window edge for a window anchor).
    float gap = 0.0f;
    // Take the opposite side when the chosen one does not fit and the other has more room.
    bool flip = true;
    // Slide along each axis to stay inside the viewport.
    bool shift = true;
};

struct PlacementInput {
    // The anchor in window logical pixels; for a window anchor, the window itself.
    rendering::rhi::Rect anchor;
    bool anchor_is_window = false;
    float width = 0.0f; // the box to place
    float height = 0.0f;
    // The safe rectangle: the window minus bars, cutout, IME, gesture zones and hinge.
    rendering::rhi::Rect viewport;
    OverlayPlacement placement;
    bool rtl = false;
};

struct PlacementResult {
    // Where the box goes, in window logical pixels.
    rendering::rhi::Rect rect;
    // The side actually used, after any flip (logical: start and end are not swapped back).
    OverlaySide side = OverlaySide::Bottom;
    bool flipped = false;
    bool shifted = false;
};

// Non-finite or negative inputs count as zero; a box larger than the viewport sits at its start
// edge rather than centring half outside.
[[nodiscard]] PlacementResult compute_placement(const PlacementInput& input) noexcept;

} // namespace tessera::ui
