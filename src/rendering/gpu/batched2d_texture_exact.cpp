// SPDX-License-Identifier: MIT

// The exact-nearest textured quad of the batched 2D renderer (issue #682). Kept apart from
// `batched2d_renderer.cpp`, which is at its size budget.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::rendering::gpu {

void Batched2DRenderer::push_texture_exact(const rhi::Rect& rect, rhi::Texture& texture,
                                           rhi::Color color) {
    if (rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    // The exact mapping is defined for an axis-aligned, unflipped quad: the shader works from an
    // integer device rectangle. A rotated, skewed or mirrored transform has no such rectangle, so
    // the quad falls back to the texture's own sampler (an image under a rotation is resampled
    // anyway, so there is no CPU rule left to match).
    const float* m = transform_.m;
    constexpr float kAxisEpsilon = 1.0e-6f;
    if (std::abs(m[1]) > kAxisEpsilon || std::abs(m[3]) > kAxisEpsilon || !(m[0] > 0.0f) ||
        !(m[4] > 0.0f)) {
        push_texture(rect, texture, rhi::Rect{0.0f, 0.0f, 1.0f, 1.0f}, color);
        return;
    }
    // The fragment shader needs the quad's integer rectangle in target pixels, measured the way
    // `emit_quad` places its vertices: transform first, then the content scale.
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    transform_.transform_point(rect.x, rect.y, x0, y0);
    transform_.transform_point(rect.right(), rect.bottom(), x1, y1);
    x0 *= content_scale_x_;
    x1 *= content_scale_x_;
    y0 *= content_scale_y_;
    y1 *= content_scale_y_;

    ShadowCarveBox device_rect;
    device_rect.x = std::round(std::min(x0, x1));
    device_rect.y = std::round(std::min(y0, y1));
    device_rect.half_width = std::round(std::abs(x1 - x0));
    device_rect.half_height = std::round(std::abs(y1 - y0));
    emit_quad(rect, rhi::Rect{0.0f, 0.0f, 1.0f, 1.0f}, color, 0.0f, kExactNearestMode, &texture,
              device_rect);
}

} // namespace tessera::rendering::gpu
