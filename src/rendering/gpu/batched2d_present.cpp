// SPDX-License-Identifier: MIT
//
// The 4x presentation seam of Batched2DRenderer (docs/design/display-aware-rendering.md section 4,
// S8), in its own translation unit so the renderer core stays inside its size budget.
//
// A multisampled render target is not a copy or blit source, so the window cannot present a 4x
// frame the way it resamples an adaptive render-scale target. RHI 1.3's sampled view of a render
// target is the GPU-only alternative: the target's resolved image is bound and drawn into the
// single-sample frame target by one full-target quad on a single-sample, no-blend pipeline.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace tessera::rendering::gpu {

void Batched2DRenderer::present_target(rhi::CommandList& commands,
                                       const rhi::RenderTarget& destination,
                                       const rhi::RenderTargetDesc& destination_desc,
                                       const rhi::RenderTarget& source, rhi::Pipeline& pipeline) {
    if (in_frame_) {
        throw std::logic_error(
            "batched2d present_target: the frame is still open; call end() first");
    }
    if (source.presentable()) {
        throw std::invalid_argument(
            "batched2d present_target: a swapchain image is not a sampling source");
    }
    const rhi::Extent2D extent = destination.extent();
    if (destination_desc.extent.width != extent.width ||
        destination_desc.extent.height != extent.height) {
        throw std::invalid_argument(
            "batched2d present_target: descriptor extent does not match the destination");
    }
    if (destination.sample_count() != 1U || destination_desc.sample_count != 1U ||
        pipeline.sample_count() != 1U) {
        throw std::invalid_argument(
            "batched2d present_target: the present pass is single-sampled (target " +
            std::to_string(destination.sample_count()) + ", desc " +
            std::to_string(destination_desc.sample_count) + ", pipeline " +
            std::to_string(pipeline.sample_count()) + ")");
    }
    if (source.format() != destination.format()) {
        throw std::invalid_argument(
            "batched2d present_target: source and destination formats differ");
    }
    if (extent.width == 0 || extent.height == 0) {
        return;
    }

    // One quad covering the whole destination with uv 0..1. The source target's own sampler state
    // stretches it, so a 1:1 presentation is exact and an adaptive render-scale presentation
    // resamples exactly like blit_target(Linear) does for a 1-sample target.
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    const auto stage = [this](float x, float y, float u, float v) {
        Vertex2D vertex;
        vertex.x = x;
        vertex.y = y;
        vertex.u = u;
        vertex.v = v;
        vertex.color = 0xFFFFFFFFu;
        vertex.mode = 2.0f; // textured quad
        vertices_.push_vertex(vertex);
    };
    stage(0.0f, 0.0f, 0.0f, 0.0f);
    stage(width, 0.0f, 1.0f, 0.0f);
    stage(width, height, 1.0f, 1.0f);
    stage(0.0f, 0.0f, 0.0f, 0.0f);
    stage(width, height, 1.0f, 1.0f);
    stage(0.0f, height, 0.0f, 1.0f);
    const VertexArena::Span span = vertices_.upload();
    vertices_.clear();
    if (span.buffer == nullptr || span.vertex_count == 0) {
        return;
    }

    Batched2DPushConstants push;
    push.viewport_width = width;
    push.viewport_height = height;
    // No SDF edge and no clip: the pass replaces every destination pixel with the source's. Set 0
    // is the empty clip set, so the shader's clip loop does not run.
    push.aa_radius = 0.0f;
    push.clip_set = 0;

    // RHI 1.3 ordering: the sampled bind is recorded outside a render pass and after a pipeline
    // bind, before the pass that reads the target opens.
    commands.bind_pipeline(pipeline);
    commands.bind_sampled_target(0, source);
    commands.begin_render_pass(destination, destination_desc);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, width, height});
    commands.set_scissor(rhi::Scissor{0, 0, extent.width, extent.height});
    commands.push_constants(&push, sizeof(push));
    commands.bind_vertex_buffer(*span.buffer);
    commands.draw(span.vertex_count, span.base_vertex);
    commands.end_render_pass();
    ++draw_calls_;
}

} // namespace tessera::rendering::gpu
