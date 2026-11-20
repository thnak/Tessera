// SPDX-License-Identifier: MIT

// The two box-shadow quads of the batched 2D renderer: the outer shadow (issue #461) and the inset
// shadow (issue #612). They share the carve attribute and the Gaussian edge, so they live together
// and away from `batched2d_renderer.cpp`, which is at its size budget.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>

namespace tessera::rendering::gpu {

void Batched2DRenderer::push_soft_shadow(const rhi::Rect& box, const rhi::Rect& carve,
                                         rhi::Color color, float radius, float sigma) {
    if (box.width <= 0.0f || box.height <= 0.0f) {
        return;
    }
    // sigma 0 is the hard-edged shadow: the same quad and carve test, with a coverage step in the
    // shader instead of the Gaussian (one path for every outer shadow, issue #461).
    const float clamped_sigma = std::max(sigma, 0.0f);
    const float reach = kSoftShadowReach * clamped_sigma;
    const float clamped_radius = std::clamp(radius, 0.0f, std::min(box.width, box.height) * 0.5f);
    const rhi::Rect quad{box.x - reach, box.y - reach, box.width + 2.0f * reach,
                         box.height + 2.0f * reach};
    // The carve box in the quad's local frame. The shader subtracts it (rounded) so the shadow is
    // painted only outside the element's border box; an empty carve subtracts nothing.
    ShadowCarveBox carved;
    if (carve.width > 0.0f && carve.height > 0.0f) {
        carved.x = (carve.x + carve.width * 0.5f) - (quad.x + quad.width * 0.5f);
        carved.y = (carve.y + carve.height * 0.5f) - (quad.y + quad.height * 0.5f);
        carved.half_width = carve.width * 0.5f;
        carved.half_height = carve.height * 0.5f;
    }
    emit_quad(quad, rhi::Rect{}, color, clamped_radius, kSoftShadowModeBase + clamped_sigma,
              white_texture_, carved);
}

void Batched2DRenderer::push_inset_shadow(const rhi::Rect& box, const rhi::Rect& carve,
                                          rhi::Color color, float radius, float sigma) {
    if (box.width <= 0.0f || box.height <= 0.0f) {
        return;
    }
    // The quad is the border box itself: an inset shadow never paints outside it, so there is no
    // Gaussian reach to grow by. The shader clips the coverage to the quad and fades it inward from
    // the inner box, which is why the reach is not part of the geometry here.
    const float clamped_sigma = std::max(sigma, 0.0f);
    const float clamped_radius = std::clamp(radius, 0.0f, std::min(box.width, box.height) * 0.5f);
    // The inner box in the quad's local frame: the shader subtracts it so the shadow is painted
    // only outside it (inside the border box). An empty carve paints the whole box, which is what a
    // spread that consumes the box degenerates to.
    ShadowCarveBox carved;
    if (carve.width > 0.0f && carve.height > 0.0f) {
        carved.x = (carve.x + carve.width * 0.5f) - (box.x + box.width * 0.5f);
        carved.y = (carve.y + carve.height * 0.5f) - (box.y + box.height * 0.5f);
        carved.half_width = carve.width * 0.5f;
        carved.half_height = carve.height * 0.5f;
    }
    emit_quad(box, rhi::Rect{}, color, clamped_radius, kInsetShadowModeBase + clamped_sigma,
              white_texture_, carved);
}

} // namespace tessera::rendering::gpu
