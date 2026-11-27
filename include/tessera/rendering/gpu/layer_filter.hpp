// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {

// One resolved `filter` function of an isolated layer (issue #617, docs/design/gpu/
// offscreen-layers.md section 5.2): what the batched renderer runs on the layer's pixels before
// the mask and the group opacity. Lengths are CSS px in paint coordinates; the renderer applies
// its own content scale, and the paint pass has already clamped `sigma` to `glassMaxBlurRadius`.
struct LayerFilterOp {
    enum class Kind : std::uint8_t {
        Blur,       // Gaussian blur of the whole layer, standard deviation `sigma`
        DropShadow, // the layer's alpha, offset, blurred and tinted, composited below the layer
    };
    Kind kind = Kind::Blur;
    float offset_x = 0.0f; // DropShadow only
    float offset_y = 0.0f;
    float sigma = 0.0f;
    rhi::Color color{0.0f, 0.0f, 0.0f, 1.0f}; // DropShadow only, straight alpha
};

// A short fixed list: it rides inside `LayerDraw`, so the paint cache copies it without allocating.
// Adjacent `blur()`s fold into one (variances add), so three covers blur/shadow/blur chains.
inline constexpr std::size_t kMaxLayerFilterOps = 3;

struct LayerFilterChain {
    std::array<LayerFilterOp, kMaxLayerFilterOps> ops{};
    std::uint8_t count = 0;

    [[nodiscard]] bool empty() const {
        return count == 0;
    }
    bool push(const LayerFilterOp& op) {
        if (count >= kMaxLayerFilterOps) {
            return false;
        }
        ops[count++] = op;
        return true;
    }
};

} // namespace tessera::rendering::gpu
