// SPDX-License-Identifier: MIT

// Positioning of overlay content against its anchor (docs/specs/overlays-and-layers-v1.md
// section 3, issue #302). Runs after layout, once per frame.

#include "overlay_entry.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui {

namespace {

using overlay_detail::find_node;

// Offsets closer than this are the same position: it keeps a settled overlay from rewriting its
// inline style, and so from asking for layout, every frame.
constexpr float kMovedPixels = 0.005f;

} // namespace

bool OverlayHost::has_placed() const {
    return std::ranges::any_of(entries_, [](const auto& entry) {
        return entry->state == Entry::State::Open && !entry->closing && entry->spec.placement;
    });
}

OverlayHost::PlaceResult OverlayHost::place(const rendering::rhi::Rect& viewport, bool rtl) {
    PlaceResult result;
    for (const auto& owned : entries_) {
        Entry& entry = *owned;
        if (entry.state != Entry::State::Open || entry.closing || !entry.spec.placement ||
            entry.content == nullptr) {
            continue;
        }
        const OverlayPlacement& want = *entry.spec.placement;
        PlacementInput input;
        input.placement = want;
        input.viewport = viewport;
        input.rtl = rtl;
        switch (want.anchor.kind) {
        case OverlayAnchor::Kind::Window:
            input.anchor_is_window = true;
            break;
        case OverlayAnchor::Kind::Rect:
            input.anchor = want.anchor.rect;
            break;
        case OverlayAnchor::Kind::Node: {
            const Node* anchor = find_node(root_, want.anchor.node);
            if (anchor == nullptr) {
                // The default is to close; re-anchoring by a stable id is opt-in and not built.
                entry.closing = true;
                result.closed = true;
                continue;
            }
            input.anchor = anchor->world_bounds();
            break;
        }
        }
        const rendering::rhi::Rect size = entry.content->world_bounds();
        input.width = size.width;
        input.height = size.height;
        const PlacementResult placed = compute_placement(input);
        // The wrapper sits at the window origin, so window coordinates are the inline offsets, and
        // a laid-out content box already reports where the last offsets put it.
        const rendering::rhi::Rect& current = entry.content->bounds();
        if (entry.placed && std::fabs(placed.rect.x - current.x) < kMovedPixels &&
            std::fabs(placed.rect.y - current.y) < kMovedPixels) {
            continue;
        }
        entry.placed = true;
        entry.content->set_inline_style("left", entry.left.write(placed.rect.x));
        entry.content->set_inline_style("top", entry.top.write(placed.rect.y));
        result.moved = true;
    }
    return result;
}

} // namespace tessera::ui
