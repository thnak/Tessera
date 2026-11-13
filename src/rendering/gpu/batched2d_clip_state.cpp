// SPDX-License-Identifier: MIT
//
// Clip-state interning and push-constant packing for Batched2DRenderer. Split out of
// batched2d_renderer.cpp to keep that file under the 500-line hard limit (AGENTS.md, code
// conventions); the definitions are the same class members, just in a second translation unit.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <cstdint>

namespace tessera::rendering::gpu {

bool Batched2DRenderer::same_clip_state(const ClipState& a, const ClipState& b) {
    if (a.count != b.count) {
        return false;
    }
    for (std::uint32_t i = 0; i < a.count; ++i) {
        if (!(a.clips[i] == b.clips[i])) {
            return false;
        }
    }
    return true;
}

std::uint32_t Batched2DRenderer::intern_clip_state(const ClipState& state) {
    for (std::uint32_t i = 0; i < clip_states_.size(); ++i) {
        if (same_clip_state(clip_states_[i], state)) {
            return i;
        }
    }
    clip_states_.push_back(state);
    return static_cast<std::uint32_t>(clip_states_.size() - 1);
}

void Batched2DRenderer::rebuild_clip_state() {
    ClipState folded;
    for (const clip::RoundedRect& active : clip_stack_) {
        if (folded.count < clip::kMaxShaderClips) {
            folded.clips[folded.count++] = active;
        } else {
            // Deeper clips fold into the innermost shader slot conservatively; the CPU-side scissor
            // still reflects every clip rect, so the result never paints outside any active clip.
            folded.clips[clip::kMaxShaderClips - 1] =
                clip::intersect_clips(folded.clips[clip::kMaxShaderClips - 1], active);
        }
    }
    current_clip_state_ = intern_clip_state(folded);
}

void Batched2DRenderer::update_push_constants(std::uint32_t state_index) {
    Batched2DPushConstants push;
    push.viewport_width = static_cast<float>(extent_.width);
    push.viewport_height = static_cast<float>(extent_.height);
    push.aa_radius = (config_.enable_aa && !config_.golden_mode) ? 1.0f : 0.0f;
    const ClipState& state = clip_states_[state_index];
    push.clip_count = state.count;
    for (std::uint32_t i = 0; i < state.count; ++i) {
        const clip::RoundedRect& source = state.clips[i];
        Batched2DPushConstants::ClipUniform& target = push.clips[i];
        target.x = source.rect.x;
        target.y = source.rect.y;
        target.width = source.rect.width;
        target.height = source.rect.height;
        target.top_left = source.top_left;
        target.top_right = source.top_right;
        target.bottom_right = source.bottom_right;
        target.bottom_left = source.bottom_left;
    }
    commands_->push_constants(&push, sizeof(push));
}

} // namespace tessera::rendering::gpu
