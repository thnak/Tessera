// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tessera::rendering::gpu {

// The rounded-clip stack of the batched 2D renderer and the interned fold of it that the fragment
// shader reads (god-type audit slice 8; the former batched2d_clip_state.cpp).
//
// Two levels of state, for two different reasons:
//
//  - The *stack* is the caller-visible nesting: push_clip()/pop_clip() must pair, deeper nesting is
//    folded conservatively, and every level remembers the scissor it was entered with so a pop can
//    restore it.
//  - The *interned fold* is what batching sees. A batch stores only a state index, so two draws
//    with the same folded uniform set share one and the batch does not split between them. State 0
//    is always the empty set, which is why an unclipped batch needs no special case.
//
// The class also packs the push-constant block, because that is the only consumer of the fold and
// keeping the byte layout in one place is what stops the two from drifting.
class ClipStateTable {
public:
    struct State {
        std::uint32_t count = 0;
        std::array<clip::RoundedRect, clip::kMaxShaderClips> clips{};

        [[nodiscard]] bool equals(const State& other) const;
    };

    // The result of a stack change: the interned state that is current now, and the scissor the
    // renderer should apply for it.
    struct Update {
        std::uint32_t state = 0;
        rhi::Scissor scissor;
    };

    ClipStateTable();

    // Drops the stack and every interned state but the empty set, ready for a new frame.
    void reset();

    // Pushes one already-clamped clip. `current` is the renderer's live scissor (a damage region, a
    // scroll clip and any enclosing clip), which the stack cannot know by itself; recording it per
    // level is what lets a pop restore exactly that value. The caller applies content scale and
    // radius clamping, since both are its own frame state.
    [[nodiscard]] Update push(const clip::RoundedRect& clip, rhi::Scissor current);

    // Pops one level. `current` is the renderer's live scissor, used only when the stack is
    // unbalanced and there is nothing to restore.
    [[nodiscard]] Update pop(rhi::Scissor current);

    [[nodiscard]] std::uint32_t index() const {
        return current_state_;
    }
    [[nodiscard]] std::uint32_t depth() const {
        return static_cast<std::uint32_t>(clip_stack_.size());
    }
    [[nodiscard]] bool empty() const {
        return clip_folded_.empty();
    }

    // The interned state at `index`; state 0 when the index is out of range, so a stale index can
    // never read past the table.
    [[nodiscard]] const State& state(std::uint32_t index) const;

    // Packs `index` into the batch push-constant block. Pure: it reads the table and the frame
    // viewport, so the caller decides when to record the constant write.
    void pack_push_constants(std::uint32_t index, rhi::Extent2D viewport, float aa_radius,
                             Batched2DPushConstants& out) const;

    [[nodiscard]] std::uint32_t interned_count() const {
        return static_cast<std::uint32_t>(states_.size());
    }

private:
    // Recomputes the fold from the stack and interns it.
    void rebuild();
    std::uint32_t intern(const State& state);

    std::vector<clip::RoundedRect> clip_stack_;
    // One entry per pushed clip: true when it was folded into the innermost slot instead of being
    // pushed onto clip_stack_, so pop() knows whether to unwind the stack itself.
    std::vector<bool> clip_folded_;
    // The scissor in effect before each push, so a pop restores exactly what the caller had.
    std::vector<rhi::Scissor> scissor_stack_;
    std::vector<State> states_;
    std::uint32_t current_state_ = 0;
};

} // namespace tessera::rendering::gpu
