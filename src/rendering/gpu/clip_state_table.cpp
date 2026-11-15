// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/clip_state_table.hpp"

#include <algorithm>

namespace tessera::rendering::gpu {

bool ClipStateTable::State::equals(const State& other) const {
    if (count != other.count) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!(clips[i] == other.clips[i])) {
            return false;
        }
    }
    return true;
}

ClipStateTable::ClipStateTable() {
    // State 0 is the empty clip set, so index 0 always means "no shader clips" and an unclipped
    // batch needs no special case in the renderer.
    states_.reserve(16);
    states_.push_back(State{});
    clip_stack_.reserve(clip::kMaxClipDepth);
    clip_folded_.reserve(clip::kMaxClipDepth);
    scissor_stack_.reserve(clip::kMaxClipDepth);
}

void ClipStateTable::reset() {
    clip_stack_.clear();
    clip_folded_.clear();
    scissor_stack_.clear();
    states_.resize(1);
    current_state_ = 0;
}

ClipStateTable::Update ClipStateTable::push(const clip::RoundedRect& clip, rhi::Scissor current) {
    // The enclosing scissor is recorded per level: the renderer may have a damage or scroll scissor
    // narrower than the clip, and a pop must restore whatever was in effect rather than the
    // frame's full-target rect.
    scissor_stack_.push_back(current);
    if (clip_stack_.size() < clip::kMaxClipDepth) {
        clip_stack_.push_back(clip);
        clip_folded_.push_back(false);
    } else {
        // Documented nesting bound: deeper clips are folded into the innermost active clip
        // (conservative intersection) instead of growing the stack.
        clip_stack_.back() = clip::intersect_clips(clip_stack_.back(), clip);
        clip_folded_.push_back(true);
    }
    rebuild();
    return Update{current_state_, rhi::intersect(current, clip::enclosing_scissor(clip))};
}

ClipStateTable::Update ClipStateTable::pop(rhi::Scissor current) {
    if (clip_folded_.empty()) {
        // Unbalanced pop: nothing to restore, so the caller's scissor stands.
        return Update{current_state_, current};
    }
    const bool folded = clip_folded_.back();
    clip_folded_.pop_back();
    if (!folded && !clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
    // The recorded scissor already accounted for every enclosing clip, so restoring it is exact.
    const rhi::Scissor restored = scissor_stack_.empty() ? current : scissor_stack_.back();
    if (!scissor_stack_.empty()) {
        scissor_stack_.pop_back();
    }
    rebuild();
    return Update{current_state_, restored};
}

void ClipStateTable::rebuild() {
    State folded;
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
    current_state_ = intern(folded);
}

std::uint32_t ClipStateTable::intern(const State& state) {
    for (std::uint32_t i = 0; i < states_.size(); ++i) {
        if (states_[i].equals(state)) {
            return i;
        }
    }
    states_.push_back(state);
    return static_cast<std::uint32_t>(states_.size() - 1);
}

const ClipStateTable::State& ClipStateTable::state(std::uint32_t index) const {
    return index < states_.size() ? states_[index] : states_[0];
}

void ClipStateTable::pack_push_constants(std::uint32_t index, rhi::Extent2D viewport,
                                         float aa_radius, Batched2DPushConstants& out) const {
    out.viewport_width = static_cast<float>(viewport.width);
    out.viewport_height = static_cast<float>(viewport.height);
    out.aa_radius = aa_radius;
    const State& folded = state(index);
    out.clip_count = folded.count;
    for (std::uint32_t i = 0; i < folded.count; ++i) {
        const clip::RoundedRect& source = folded.clips[i];
        Batched2DPushConstants::ClipUniform& target = out.clips[i];
        target.x = source.rect.x;
        target.y = source.rect.y;
        target.width = source.rect.width;
        target.height = source.rect.height;
        target.top_left = source.top_left;
        target.top_right = source.top_right;
        target.bottom_right = source.bottom_right;
        target.bottom_left = source.bottom_left;
    }
}

} // namespace tessera::rendering::gpu
