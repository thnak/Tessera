// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {

// One resolved `filter` function of an isolated layer (issue #617, docs/design/gpu/
// offscreen-layers.md section 5.2): what the batched renderer runs on the layer's pixels before
// the mask and the group opacity. Lengths are CSS px in paint coordinates; the renderer applies
// its own content scale, and the paint pass has already clamped `sigma` to `glassMaxBlurRadius`.
//
// The op is 20 bytes on purpose: a chain rides inside `LayerDraw`, which the paint cache stores in
// every `PaintOp` (budget: paint_op_size), so the shadow colour is packed 8 bits per channel,
// which is all precision a CSS colour has.
struct LayerFilterOp {
    enum class Kind : std::uint8_t {
        Blur,       // Gaussian blur of the whole layer, standard deviation `sigma`
        DropShadow, // the layer's alpha, offset, blurred and tinted, composited below the layer
    };
    float offset_x = 0.0f; // DropShadow only
    float offset_y = 0.0f;
    float sigma = 0.0f;
    std::uint32_t rgba = 0x000000ffu; // DropShadow only: 0xRRGGBBAA, straight alpha
    Kind kind = Kind::Blur;

    void set_tint(const rhi::Color& color) {
        const auto byte = [](float value) {
            return static_cast<std::uint32_t>(
                std::lround(std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f) * 255.0f));
        };
        rgba = byte(color.r) << 24 | byte(color.g) << 16 | byte(color.b) << 8 | byte(color.a);
    }
    [[nodiscard]] rhi::Color tint() const {
        return rhi::Color{static_cast<float>((rgba >> 24) & 0xffu) / 255.0f,
                          static_cast<float>((rgba >> 16) & 0xffu) / 255.0f,
                          static_cast<float>((rgba >> 8) & 0xffu) / 255.0f,
                          static_cast<float>(rgba & 0xffu) / 255.0f};
    }
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

static_assert(sizeof(LayerFilterChain) <= 64, "a filter chain rides in every PaintOp's LayerDraw");

} // namespace tessera::rendering::gpu
