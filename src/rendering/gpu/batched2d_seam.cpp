// SPDX-License-Identifier: MIT
//
// The isolated-layer seam of Batched2DRenderer (issue #242, docs/design/gpu/offscreen-layers.md):
// a render-pass break and resume that keeps the frame target, the bounded offscreen target pool
// that a backdrop copy (#244) or a full subtree layer (#227) renders into, and the glass backdrop
// composite that drives both.
//
// A tree that never calls break_pass() records exactly the commands it did before this file
// existed: the seam only runs when a caller opts in, which is what keeps the existing goldens
// byte-identical.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <cstdint>

namespace tessera::rendering::gpu {

void Batched2DRenderer::break_pass() {
    if (!in_frame_ || pass_broken_ || commands_ == nullptr) {
        return;
    }
    // Every draw recorded so far must reach the command list before the pass closes; a pending
    // batch would otherwise be emitted into a closed pass by the next flush().
    flush();
    commands_->end_render_pass();
    pass_broken_ = true;
}

void Batched2DRenderer::bind_frame_state() {
    // A new pass starts with no bound pipeline, viewport, scissor or push constants, and a glass
    // composite records its own pipeline/push constants in the middle of the frame's pass, so the
    // frame state is re-established from the renderer's own stacks rather than replayed.
    bound_pipeline_ = pipeline_;
    commands_->bind_pipeline(*pipeline_);
    commands_->set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                                          static_cast<float>(extent_.height)});
    commands_->set_scissor(scissor_);
    update_push_constants(clips_.index());
}

void Batched2DRenderer::resume_pass() {
    if (!in_frame_ || !pass_broken_ || commands_ == nullptr || frame_target_ == nullptr) {
        return;
    }
    rhi::RenderTargetDesc desc = frame_desc_;
    // Load preserves what the target already holds: the frame continues, it does not restart.
    desc.load_op = rhi::LoadOp::Load;
    commands_->begin_render_pass(*frame_target_, desc);
    // The renderer's own state (transform, clips, scissor stack, content scale, vertex buffer) is
    // untouched, so geometry after the break continues exactly where the pre-break stream stopped.
    bind_frame_state();
    pass_broken_ = false;
}

rhi::RenderTarget* Batched2DRenderer::acquire_offscreen_target(rhi::Extent2D extent) {
    if (!in_frame_) {
        return nullptr;
    }
    return glass_.acquire(extent);
}

void Batched2DRenderer::release_offscreen_target(const rhi::RenderTarget* target) {
    if (in_frame_) {
        glass_.release(target);
    }
}

std::uint32_t Batched2DRenderer::offscreen_target_count() const {
    return glass_.target_count();
}

OffscreenPoolStats Batched2DRenderer::offscreen_stats() const {
    return glass_.pool_stats();
}

void Batched2DRenderer::trim_offscreen_targets(bool all_complete) {
    if (!in_frame_) {
        if (retain_.cache != nullptr) {
            retain_.cache->trim(all_complete);
        }
        glass_.trim(all_complete);
    }
}

bool Batched2DRenderer::glass_supported() const {
    // Deliberately not frame-scoped: the paint pass asks this while planning, before begin().
    // draw_backdrop() applies the frame checks itself.
    return glass_.supported();
}

std::uint32_t Batched2DRenderer::glass_draws() const {
    return glass_.draws();
}

bool Batched2DRenderer::draw_backdrop(const GlassBackdropDraw& draw) {
    if (!in_frame_ || commands_ == nullptr || frame_target_ == nullptr || !glass_.supported()) {
        return false;
    }

    GlassCompositor::FrameContext frame;
    frame.commands = commands_;
    frame.target = frame_target_;
    frame.desc = frame_desc_;
    frame.extent = extent_;
    frame.scissor = scissor_;
    frame.content_scale_x = content_scale_x_;
    frame.content_scale_y = content_scale_y_;
    // The compositor reopens the pass between its blur and composite passes through this seam, so
    // the renderer keeps ownership of the frame's pass state.
    frame.seam = GlassCompositor::PassSeam{this, &Batched2DRenderer::resume_pass_callback};

    // The pass is broken before the compositor touches the pool, so a failed composite leaves the
    // frame resumed and the caller's recording continues either way.
    break_pass();
    // The composite binds its own pipeline, so whatever this renderer last bound is no longer
    // current; the next flush must rebind rather than assume.
    bound_pipeline_ = nullptr;
    if (!glass_.compose(frame, draw)) {
        resume_pass();
        return false;
    }
    // The composite bound its own pipeline and push constants inside the frame's pass: restore the
    // 2D state so geometry recorded after the glass node continues the pre-break stream.
    bind_frame_state();
    return true;
}

} // namespace tessera::rendering::gpu
