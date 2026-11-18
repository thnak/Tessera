// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"

#include <algorithm>
#include <stdexcept>

// The LCD text half of the batched renderer (display-aware-rendering.md section 3.4.1, step 3),
// split out of batched2d_renderer.cpp so that file stays inside its size budget.
namespace tessera::rendering::gpu {

void Batched2DRenderer::set_lcd_pipeline(rhi::Pipeline* pipeline) {
    lcd_pipeline_ = pipeline;
}

void Batched2DRenderer::set_texture_array_pipeline(rhi::Pipeline* pipeline) {
    // A device without the capability cannot honour the request, and only a pipeline built for the
    // array layout can bind one. Refuse instead of silently keeping the old path.
    if (pipeline != nullptr && device_->texture_array_capacity() == 0) {
        throw std::invalid_argument("set_texture_array_pipeline: this device has no texture array");
    }
    array_pipeline_ = pipeline;
}

void Batched2DRenderer::push_texture_lcd(const rhi::Rect& rect, rhi::Texture& texture,
                                         const rhi::Rect& uv, rhi::Color color,
                                         float coverage_boost, bool per_channel) {
    if (lcd_pipeline_ == nullptr) {
        push_texture(rect, texture, uv, color, coverage_boost);
        return;
    }
    const float boost = std::clamp(coverage_boost, 1.0f, 2.0f);
    quad_pipeline_ = lcd_pipeline_;
    // The radius slot is unused by a textured quad; the LCD program reads 1 as "one coverage".
    emit_quad(rect, uv, color, per_channel ? 0.0f : 1.0f,
              2.0f + (boost - 1.0f) / kCoverageBoostScale, &texture);
    quad_pipeline_ = nullptr;
}

void Batched2DRenderer::bind_batch_pipeline(const Batch& batch, rhi::Pipeline*& bound) {
    // An LCD batch keeps its own pipeline; a texture-array batch needs the descriptor-indexing
    // pipeline, because its layout is the one that declares the sampled-image array (RHI 1.7, issue
    // #480 part 2). Everything else draws with the frame pipeline.
    rhi::Pipeline* wanted = batch.pipeline != nullptr ? batch.pipeline
                            : batch.texture_array     ? array_pipeline_
                                                      : pipeline_;
    if (wanted != bound) {
        commands_->bind_pipeline(*wanted);
        bound = wanted;
    }
}

} // namespace tessera::rendering::gpu
