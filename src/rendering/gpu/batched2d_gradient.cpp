// SPDX-License-Identifier: MIT

// The per-fragment gradient quad of the batched 2D renderer (docs/design/gpu/gradients.md,
// ADR-076). Kept apart from `batched2d_renderer.cpp`, which is at its size budget.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

namespace tessera::rendering::gpu {

bool Batched2DRenderer::gradients_supported() const {
    // Both halves matter: the device must ship a batched2d program with the gradient branch, and
    // the frame pipeline must declare the record buffer the branch reads. A fixture pipeline
    // created without `PipelineDesc::storage_buffer` has no set 1, so its shader cannot evaluate
    // one.
    return config_.analytic_gradients && device_ != nullptr &&
           device_->supports_analytic_gradients() && pipeline_ != nullptr &&
           pipeline_->declares_storage_buffer();
}

bool Batched2DRenderer::push_gradient(const gradient::Fill& fill) {
    if (!gradients_supported()) {
        ++gradient_fallbacks_;
        return false;
    }
    if (!in_frame_) {
        return true;
    }
    // Before the record is written, so a culled gradient takes no table space and splits no batch.
    if (cull_quad(fill.rect)) {
        return true;
    }
    const std::optional<std::uint32_t> index = gradients_.add(fill);
    if (!index.has_value()) {
        ++gradient_fallbacks_;
        return false;
    }
    // The quad is white: the shader multiplies the ramp by the vertex colour, which carries only
    // the flat-layer opacity that emit_quad folds into every alpha. The record index rides in the
    // carve box, which nothing else uses on a gradient quad; it is exact as a float up to 2^24.
    ShadowCarveBox record;
    record.x = static_cast<float>(*index);
    emit_quad(fill.rect, rhi::Rect{}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f,
              gradient::kGradientMode, white_texture_, record);
    ++gradient_quads_;
    return true;
}

} // namespace tessera::rendering::gpu
