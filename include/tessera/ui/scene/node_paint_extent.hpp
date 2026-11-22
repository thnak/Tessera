// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node_types.hpp"

#include <cmath>

// How far a scene node's pixels reach beyond its border box, and the cached union of that reach over
// its subtree (docs/architecture/god-type-audit-v1.md section 3.1; issue #735 slice C): the
// box-shadow outsets, the glass blur sigma and the subtree paint box the paint cull and the hit-test
// prune read. 40 bytes, on demand - a pass reads the cached box of a node, not the inputs.
//
// The type owns the inputs, their sanitizing and the cache slot. It does not compute the box: that
// needs the children, which only `ui::Node` can reach (`subtree_paint_bounds()` in
// `node_geometry.cpp`). The cache is `mutable` because paint fills it lazily through a const scene
// (issue #733 item 4). Width encodes its state: negative is "not computed", +infinity is "computed
// and unbounded", else the box - so no separate validity flag is needed.
namespace tessera::ui {

class NodePaintExtent {
public:
    // Negative and non-finite values count as 0. Returns true when a stored value changed.
    bool set_outsets(const PaintOutsets& value) noexcept {
        const PaintOutsets next{clean(value.left), clean(value.top), clean(value.right),
                                clean(value.bottom), value.blurred};
        if (next.left == outsets_.left && next.top == outsets_.top && next.right == outsets_.right &&
            next.bottom == outsets_.bottom && next.blurred == outsets_.blurred) {
            return false;
        }
        outsets_ = next;
        return true;
    }
    [[nodiscard]] const PaintOutsets& outsets() const noexcept {
        return outsets_;
    }

    // The blur sigma in CSS px, already clamped to the configured maximum by the style sync; 0
    // means no backdrop-filter. Returns true when it changed.
    bool set_glass_blur_sigma(float sigma) noexcept {
        const float next = clean(sigma);
        if (next == glass_blur_sigma_) {
            return false;
        }
        glass_blur_sigma_ = next;
        return true;
    }
    [[nodiscard]] float glass_blur_sigma() const noexcept {
        return glass_blur_sigma_;
    }

    [[nodiscard]] bool cache_valid() const noexcept {
        return cache_.width >= 0.0f;
    }
    [[nodiscard]] const rendering::rhi::Rect& cached_box() const noexcept {
        return cache_;
    }
    void store_box(const rendering::rhi::Rect& box) const noexcept {
        cache_ = box;
    }
    void invalidate_cache() const noexcept {
        cache_ = rendering::rhi::Rect{0.0f, 0.0f, -1.0f, 0.0f};
    }

private:
    [[nodiscard]] static float clean(float value) noexcept {
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    }

    mutable rendering::rhi::Rect cache_{0.0f, 0.0f, -1.0f, 0.0f};
    PaintOutsets outsets_;
    float glass_blur_sigma_ = 0.0f;
};

static_assert(sizeof(NodePaintExtent) == 40, "NodePaintExtent is a box, four outsets and a sigma");

} // namespace tessera::ui
