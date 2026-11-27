// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::gpu {

// Per-frame culling counters, reset by every begin. `quads_emitted` counts quads that reached the
// vertex buffer; the two culled counters are quads dropped before it.
struct CullStats {
    std::uint32_t quads_emitted = 0;
    std::uint32_t quads_culled = 0;               // outside the scissor or the damage regions
    std::uint32_t quads_culled_empty_scissor = 0; // dropped because the scissor has no area
};

} // namespace tessera::rendering::gpu
