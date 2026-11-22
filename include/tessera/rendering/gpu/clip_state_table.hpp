// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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
//  - The *interned fold* is what batching sees. A batch stores only a set index, so two draws with
//    the same folded clip set share one and the batch does not split between them. Set 0 is always
//    the empty set, which is why an unclipped batch needs no special case.
//
// Since ADR-068 slice 3 the fold is a *list of records* rather than a fixed array of three rounded
// rectangles: the shader reads them from a per-frame storage buffer, so the count is bounded by
// that buffer's capacity instead of by the push-constant budget.
//
// A set owns its records rather than pointing into one shared list. That is what keeps a set stable
// across a stack change: a pop drops the sets it created without invalidating the offsets of the
// ones still interned, and the buffer layout is computed once per frame by pack_buffer().
class ClipStateTable {
public:
    // The result of a stack change: the interned set that is current now, and the scissor the
    // renderer should apply for it.
    struct Update {
        std::uint32_t set = 0;
        rhi::Scissor scissor;
    };

    ClipStateTable();

    // Drops the stack and every interned set but the empty one, ready for a new frame.
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
        return current_set_;
    }
    [[nodiscard]] std::uint32_t depth() const {
        return static_cast<std::uint32_t>(clip_stack_.size());
    }
    [[nodiscard]] bool empty() const {
        return clip_stack_.empty();
    }

    // The records of the interned set at `index`; empty when the index is out of range, so a stale
    // index can never read past the table.
    [[nodiscard]] std::span<const clip::ClipRecord> set(std::uint32_t index) const;

    // Every interned set's records concatenated in set order, which is the layout the clip buffer
    // holds: a set's range is a contiguous slice of this list. Rebuilt on demand, so a caller that
    // uploads once per batch pays one copy per batch, not one per push.
    [[nodiscard]] const std::vector<clip::ClipRecord>& flat_records() const;
    // One range per interned set, indexing `flat_records()`. Index 0 is the empty set.
    [[nodiscard]] const std::vector<clip::ClipSetRange>& set_ranges() const;

    // Packs `index` into the batch push-constant block. Pure: it reads the table and the frame
    // viewport, so the caller decides when to record the constant write.
    void pack_push_constants(std::uint32_t index, rhi::Extent2D viewport, float aa_radius,
                             Batched2DPushConstants& out) const;

    [[nodiscard]] std::uint32_t interned_count() const {
        return static_cast<std::uint32_t>(sets_.size());
    }

private:
    // Recomputes the fold from the stack and interns it.
    void rebuild();
    std::uint32_t intern(const std::vector<clip::ClipRecord>& records);
    // Rebuilds `flat_records_` and `set_ranges_` from `sets_`, once per frame after the last push.
    void flatten() const;

    std::vector<clip::RoundedRect> clip_stack_;
    // One entry per pushed clip: true when it was folded into the innermost active clip instead of
    // being pushed onto clip_stack_, so pop() knows whether to unwind the stack itself.
    std::vector<bool> clip_folded_;
    // The scissor in effect before each push, so a pop restores exactly what the caller had.
    std::vector<rhi::Scissor> scissor_stack_;
    // One record list per interned set. Set 0 is the empty list.
    std::vector<std::vector<clip::ClipRecord>> sets_;
    std::uint32_t current_set_ = 0;
    // The flattened view the clip buffer uploads, rebuilt lazily and invalidated by any stack
    // change.
    mutable std::vector<clip::ClipRecord> flat_records_;
    mutable std::vector<clip::ClipSetRange> set_ranges_;
    mutable bool flat_dirty_ = true;
};

} // namespace tessera::rendering::gpu
