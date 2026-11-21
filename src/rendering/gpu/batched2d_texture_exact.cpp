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
    // The fragment shader needs the quad's integer rectangle in target pixels, measured the way
    // `emit_quad` places its vertices: transform first, then the content scale. Two opposite
    // corners bound an axis-aligned quad, which is the only kind this primitive supports; a rotated
    // or skewed transform gets its bounding box, which is not a defined result.
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
