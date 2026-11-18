// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::gpu {

// Why a batch ended. Every flush_batch() records its cause, so a frame's draw-call count is
// attributable instead of one opaque number (issue #480 part 2 measures the texture share before
// deciding whether descriptor indexing is worth its complexity).
enum class BatchSplitReason : std::uint8_t {
    // The next quad samples a different texture. This is the split descriptor indexing would
    // remove.
    Texture,
    // The next quad draws with a different pipeline (an LCD glyph quad against the frame pipeline).
    Pipeline,
    // The scissor changed (set_scissor/push_scissor/pop_scissor, or a clip's enclosing scissor).
    Scissor,
    // The folded clip-uniform set changed, so the fragment shader's clip test would differ.
    Clip,
    // The caller asked for a flush (flush(), set_viewport(), break_pass()).
    Flush,
};

// Batch splits of one frame, reset by every begin. `total()` is the number of batches the frame
// recorded, so it equals the draw calls of a full redraw (a damaged frame replays a batch once per
// intersecting region, which is a separate counter).
struct BatchSplitStats {
    std::uint32_t texture = 0;
    std::uint32_t pipeline = 0;
    std::uint32_t scissor = 0;
    std::uint32_t clip = 0;
    std::uint32_t flush = 0;

    [[nodiscard]] std::uint32_t total() const noexcept {
        return texture + pipeline + scissor + clip + flush;
    }
};

} // namespace tessera::rendering::gpu
