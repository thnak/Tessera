// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu::gradient {

// The per-fragment gradient records (docs/design/gpu/gradients.md, ADR-076).
//
// A gradient layer is ONE quad. Its vertices carry `kGradientMode` and the index of a record in
// this table; the fragment shader reads the record and the stop ramp from the frame record buffer
// (the same read-only storage buffer the clip records live in, set 1 binding 0) and evaluates the
// gradient exactly at every pixel centre. Nothing about a gradient rides in the push-constant block
// or the vertex stream beyond the index, so a gradient quad batches with its neighbours: the batch
// key (texture, scissor, clip set) is unchanged.
//
// The gradient area follows the clip vertex pool, so no offset the clip area fixed moved:
//
//   GradientRecord[g]   { u32 kind; u32 stop_count; u32 first_stop; u32 flags;   32 bytes
//                         f32 params[4]; }
//   StopColor[s]        { f32 r, g, b, a; }                                      16 bytes
//   StopOffset[s]       { f32 offset; }                                           4 bytes
//
// std430-compatible: the same bytes are a Vulkan storage buffer and a WGSL `read-only-storage`
// binding. `first_stop` indexes both stop arrays, so a record's ramp is contiguous.
//
// `params` is in the quad's LOCAL space - pixels from the quad's centre, the same `v_local` the
// shader already interpolates for rounded boxes - so a gradient under a CSS transform rotates with
// its box and the record never depends on where the quad lands on the target:
//   Linear  params = (dx / span, dy / span, 0.5, 0)        t = dot(local, params.xy) + params.z
//   Radial  params = (cx, cy, radius_x, radius_y)          t = length((local - c) / radius)
//   Conic   params = (cx, cy, from_turn, 0)                t = fract(atan2(dx, -dy) / tau - from)
// where (dx, dy) is the CSS gradient-line direction and `span` its length across the box.
//
// Stop offsets are the ramp's own positions along t, ascending and NOT confined to 0..1 (a stop may
// sit before the line starts or past where it ends). `kFlagRepeating` makes t wrap into the span
// from the first stop's offset to the last's instead of holding the end stops, which is how
// `repeating-*-gradient()` runs with any period at no cost in stops.

enum class Kind : std::uint32_t {
    Linear = 0,
    Radial = 1,
    Conic = 2,
};

// `GradientRecord::flags` bit 0: the ramp repeats over [first offset, last offset). Written only
// when that period is positive, so the shader never divides by zero.
inline constexpr std::uint32_t kFlagRepeating = 1U;

// A gradient quad's `Vertex2D::mode`. It is above every other mode (the exact-nearest textured quad
// is 4096), so the shader tests it first, and it is a flat value: the record index rides in
// `carve_x`, which no gradient quad otherwise uses.
inline constexpr float kGradientMode = 8192.0f;

struct GradientRecord {
    std::uint32_t kind = 0;
    std::uint32_t stop_count = 0;
    std::uint32_t first_stop = 0;
    std::uint32_t flags = 0; // kFlagRepeating, else 0
    float params[4] = {};
};

static_assert(sizeof(GradientRecord) == 32, "a gradient record is 32 bytes (std430, 16-aligned)");

// The most stops one record may carry: the paint pass's `kMaxDrawStops`. A repeating ramp is not
// expanded (the flag wraps it), so an authored gradient needs at most 8. The shader's loop is
// bounded by it.
inline constexpr std::uint32_t kMaxStopsPerGradient = 32;

// Per-frame capacities. A frame that needs more keeps the gradients that fit and paints the rest
// through the banded fallback (counted in `Batched2DRenderer::gradient_fallbacks()`), so the limit
// costs fidelity on a pathological frame, never correctness or a throw. 512 records and 2048 stops
// cover about 700 three-stop gradients: well past a realistic frame.
inline constexpr std::uint32_t kMaxGradientRecords = 512;
inline constexpr std::uint32_t kMaxGradientStops = 2048;

inline constexpr std::size_t kStopColorBytes = 4 * sizeof(float);
inline constexpr std::size_t kStopOffsetBytes = sizeof(float);

// Byte offsets inside the frame record buffer. The gradient area starts where the clip area ends.
[[nodiscard]] constexpr std::size_t records_offset() {
    return clip::buffer_bytes(clip::kMaxClipRecords, clip::kMaxClipSets, clip::kMaxClipVertices);
}

[[nodiscard]] constexpr std::size_t stop_colors_offset() {
    return records_offset() +
           static_cast<std::size_t>(kMaxGradientRecords) * sizeof(GradientRecord);
}

[[nodiscard]] constexpr std::size_t stop_offsets_offset() {
    return stop_colors_offset() + static_cast<std::size_t>(kMaxGradientStops) * kStopColorBytes;
}

// The whole frame record buffer: the clip area plus the gradient area.
[[nodiscard]] constexpr std::size_t total_buffer_bytes() {
    return stop_offsets_offset() + static_cast<std::size_t>(kMaxGradientStops) * kStopOffsetBytes;
}

static_assert(records_offset() % 16 == 0 && stop_colors_offset() % 16 == 0 &&
                  stop_offsets_offset() % 16 == 0,
              "every gradient area is 16-byte aligned for the std430 arrays");
static_assert(total_buffer_bytes() == 53264 + 512 * 32 + 2048 * 16 + 2048 * 4,
              "the frame record buffer layout must match the documented byte count");

} // namespace tessera::rendering::gpu::gradient
