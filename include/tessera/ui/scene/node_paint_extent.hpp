// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node_types.hpp"

#include <cmath>
#include <cstdint>

// How far a scene node's pixels reach beyond its border box, and the cached union of that reach
// over its subtree (docs/architecture/god-type-audit-v1.md section 3.1; issue #735 slice C): the
// box-shadow outsets, the glass blur sigma, the layer-filter reach and the subtree paint box the
// paint cull and the hit-test prune read. 40 bytes, on demand - a pass reads the cached box of a
// node, not the inputs.
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
        if (next.left == outsets_.left && next.top == outsets_.top &&
            next.right == outsets_.right && next.bottom == outsets_.bottom &&
            next.blurred == outsets_.blurred) {
            return false;
        }
        outsets_ = next;
        return true;
    }
    [[nodiscard]] const PaintOutsets& outsets() const noexcept {
        return outsets_;
    }

    // The backdrop blur sigma in CSS px, already clamped to the configured maximum by the style
    // sync; 0 means no backdrop-filter. Returns true when the stored value changed.
    //
    // Both scalars below are 16-bit fixed point so the pair fills the four bytes the sigma used to
    // take: the node is budgeted to the byte (`kNodeSizeExpected`) and a second float would grow
    // it. Each rounds UP to its step, because a stored value that is too small would under-damage;
    // one that is a fraction of a step too large only repaints a sliver more. A value past the
    // range saturates, which is far beyond any configured blur limit or renderable texture.
    bool set_glass_blur_sigma(float sigma) noexcept {
        return assign(glass_sigma_q_, quantize(sigma, kSigmaStep));
    }
    [[nodiscard]] float glass_blur_sigma() const noexcept {
        return static_cast<float>(glass_sigma_q_) * kSigmaStep;
    }

    // How far a layer `filter` reads and writes past the node's box, in CSS px: the largest side
    // of the chain's halo (a blur grows every side by 3 sigma, a drop-shadow by 3 sigma around its
    // offset). It is the filter's own dependency, independent of the backdrop sigma above, so a
    // node can carry both and neither is mistaken for the other. 0 means no filter. Returns true
    // when the stored value changed.
    bool set_filter_reach(float reach) noexcept {
        return assign(filter_reach_q_, quantize(reach, kReachStep));
    }
    [[nodiscard]] float filter_reach() const noexcept {
        return static_cast<float>(filter_reach_q_) * kReachStep;
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
    static constexpr float kSigmaStep = 1.0f / 64.0f; // range 0..1023.98 px
    static constexpr float kReachStep = 0.25f;        // range 0..16383.75 px

    [[nodiscard]] static float clean(float value) noexcept {
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    }
    [[nodiscard]] static std::uint16_t quantize(float value, float step) noexcept {
        const float steps = std::ceil(clean(value) / step);
        return steps >= 65535.0f ? std::uint16_t{65535} : static_cast<std::uint16_t>(steps);
    }
    static bool assign(std::uint16_t& slot, std::uint16_t next) noexcept {
        if (slot == next) {
            return false;
        }
        slot = next;
        return true;
    }

    mutable rendering::rhi::Rect cache_{0.0f, 0.0f, -1.0f, 0.0f};
    PaintOutsets outsets_;
    std::uint16_t glass_sigma_q_ = 0;
    std::uint16_t filter_reach_q_ = 0;
};

static_assert(sizeof(NodePaintExtent) == 40,
              "NodePaintExtent is a box, four outsets and two 16-bit scalars");

} // namespace tessera::ui
