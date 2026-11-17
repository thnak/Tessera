// SPDX-License-Identifier: MIT

// CPU culling of the batched 2D renderer (issue #474). It is a separate translation unit so the
// renderer keeps to the frame, batch and emit lifecycle and stays within the file size rules.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>

namespace tessera::rendering::gpu {

bool Batched2DRenderer::cull_quad(const rhi::Rect& rect) {
    if (!config_.cull_draws) {
        return false;
    }
    // The pixels the next draw can reach: the active scissor (clips and scroll) inside the damage
    // regions. An empty one drops everything until the scissor widens again.
    const rhi::Scissor reach = rhi::intersect(scissor_, damage_bounds_);
    if (reach.width == 0 || reach.height == 0) {
        ++cull_stats_.quads_culled;
        ++cull_stats_.quads_culled_empty_scissor;
        return true;
    }
    // Bounds of the four transformed corners, in the same target pixels as the scissor, so a
    // rotation is covered by its axis-aligned box. Anti-aliasing stays inside the quad (the shader
    // blends inward from its edge) and a shadow's reach is already part of `rect`, so the quad's
    // own bounds are exact. A fragment is drawn only when its centre is inside the quad, hence the
    // strict comparisons: a quad merely touching the scissor edge is kept. A NaN compares false
    // everywhere and is kept, which leaves the GPU to discard it as before.
    const float corners[4][2] = {{rect.x, rect.y},
                                 {rect.right(), rect.y},
                                 {rect.right(), rect.bottom()},
                                 {rect.x, rect.bottom()}};
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    for (int i = 0; i < 4; ++i) {
        float px = 0.0f;
        float py = 0.0f;
        transform_.transform_point(corners[i][0], corners[i][1], px, py);
        px *= content_scale_x_;
        py *= content_scale_y_;
        if (i == 0) {
            min_x = max_x = px;
            min_y = max_y = py;
        } else {
            min_x = std::min(min_x, px);
            max_x = std::max(max_x, px);
            min_y = std::min(min_y, py);
            max_y = std::max(max_y, py);
        }
    }
    const float left = static_cast<float>(reach.x);
    const float top = static_cast<float>(reach.y);
    const float right = static_cast<float>(reach.x) + static_cast<float>(reach.width);
    const float bottom = static_cast<float>(reach.y) + static_cast<float>(reach.height);
    if (max_x < left || min_x > right || max_y < top || min_y > bottom) {
        ++cull_stats_.quads_culled;
        return true;
    }
    return false;
}

} // namespace tessera::rendering::gpu
