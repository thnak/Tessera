// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {

// The colour functions of CSS Filter Effects, in the order the shader and the CPU twin switch on
// them. Amounts are the computed values: brightness, contrast and saturate are unbounded factors
// (>= 0); grayscale, sepia, invert and opacity are clamped to [0, 1]; hue-rotate is radians.
enum class ColorFunction : std::uint8_t {
    None = 0,
    Brightness,
    Contrast,
    Grayscale,
    Sepia,
    Saturate,
    HueRotate,
    Invert,
    Opacity,
};

// How many colour functions one LayerFilterOp carries. Three is what 20 bytes hold (4 bytes of
// amount each plus a byte of id each) without growing a chain that rides in every PaintOp.
inline constexpr std::size_t kColorFunctionsPerOp = 3;

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
        // Up to kColorFunctionsPerOp colour functions (brightness() ... opacity()), applied in
        // order to the layer's straight-alpha colour. They reuse the other kinds' storage, so a run
        // of them costs no more than one blur: the three amounts ride in `offset_x`, `offset_y`
        // and `sigma`, and the three function ids in the low bytes of `rgba`. Use the accessors.
        ColorFunctions,
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

    // ColorFunctions only: function `index` (0..kColorFunctionsPerOp-1) and its amount. `None` ends
    // the run, so `color_count()` is the number of functions set.
    void set_color_function(std::size_t index, ColorFunction function, float amount) {
        if (kind != Kind::ColorFunctions) {
            *this = LayerFilterOp{};
            rgba = 0; // no function set yet; the default tint's alpha byte is not a function id
            kind = Kind::ColorFunctions;
        }
        if (index >= kColorFunctionsPerOp) {
            return;
        }
        amount_slot(index) = amount;
        const std::uint32_t shift = 8u * static_cast<std::uint32_t>(index);
        rgba = (rgba & ~(0xffu << shift)) | (static_cast<std::uint32_t>(function) << shift);
    }
    [[nodiscard]] ColorFunction color_function(std::size_t index) const {
        return index < kColorFunctionsPerOp
                   ? static_cast<ColorFunction>((rgba >> (8u * static_cast<std::uint32_t>(index))) &
                                                0xffu)
                   : ColorFunction::None;
    }
    [[nodiscard]] float color_amount(std::size_t index) const {
        return index < kColorFunctionsPerOp ? amount_slot(index) : 0.0f;
    }
    [[nodiscard]] std::size_t color_count() const {
        std::size_t count = 0;
        while (count < kColorFunctionsPerOp && color_function(count) != ColorFunction::None) {
            ++count;
        }
        return count;
    }

    [[nodiscard]] rhi::Color tint() const {
        return rhi::Color{static_cast<float>((rgba >> 24) & 0xffu) / 255.0f,
                          static_cast<float>((rgba >> 16) & 0xffu) / 255.0f,
                          static_cast<float>((rgba >> 8) & 0xffu) / 255.0f,
                          static_cast<float>(rgba & 0xffu) / 255.0f};
    }

private:
    [[nodiscard]] float& amount_slot(std::size_t index) {
        return index == 0 ? offset_x : index == 1 ? offset_y : sigma;
    }
    [[nodiscard]] float amount_slot(std::size_t index) const {
        return index == 0 ? offset_x : index == 1 ? offset_y : sigma;
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
