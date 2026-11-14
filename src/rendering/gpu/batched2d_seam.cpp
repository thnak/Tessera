// SPDX-License-Identifier: MIT
//
// The isolated-layer seam of Batched2DRenderer (issue #242, docs/design/gpu/offscreen-layers.md):
// a render-pass break and resume that keeps the frame target, and the bounded offscreen target
// pool that a backdrop copy (#244) or a full subtree layer (#227) renders into.
//
// A tree that never calls break_pass() records exactly the commands it did before this file
// existed: the seam only runs when a caller opts in, which is what keeps the existing goldens
// byte-identical.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/offscreen_target_pool.hpp"

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
    commands_->bind_pipeline(*pipeline_);
    commands_->set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                                          static_cast<float>(extent_.height)});
    commands_->set_scissor(scissor_);
    update_push_constants(current_clip_state_);
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
    if (!in_frame_ || offscreen_pool_ == nullptr) {
        return nullptr;
    }
    return offscreen_pool_->acquire(extent);
}

std::uint32_t Batched2DRenderer::offscreen_target_count() const {
    return offscreen_pool_ != nullptr ? offscreen_pool_->target_count() : 0;
}

} // namespace tessera::rendering::gpu
