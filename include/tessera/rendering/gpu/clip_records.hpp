// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu::clip {

// The per-frame clip-record buffer (ADR-068 slice 3, issue #671).
//
// The fragment shader used to receive at most `kMaxShaderClips` folded rounded rectangles as push
// constants, which fixed both the shape (a rect plus four circular radii) and the count. This
// buffer replaces that: a clip set is a range into a list of records, so the count is bounded by
// the buffer's capacity rather than by the push-constant budget, and a record can grow a kind
// without a new pipeline.
//
// Layout, `std430`-compatible and 16-byte aligned so the same bytes work as a Vulkan storage buffer
// and a WGSL `read-only-storage` binding:
//
//   Header      { u32 record_count; u32 reserved[3]; }                      16 bytes
//   Record[i]   { u32 kind; u32 flags; u32 first_vertex; u32 vertex_count;  64 bytes
//                 f32 x, y, width, height;
//                 f32 radii[8]; }
//   ClipSet[k]  { u32 first_record; u32 count; u32 reserved[2]; }           16 bytes
//
// Slice 3 emits `RoundedRect` records only, with the four corner radii in `radii[0..3]` and
// `radii[4..7]` zero. The elliptical pairs and the `Ellipse`/`Polygon` kinds are slice 4 and 5; the
// format already has room for them so those slices add a kind, not a layout.
//
// The buffer is one per frame in flight, written by the CPU before the frame's draws are recorded
// and read by the GPU during them. A frame must not rewrite a buffer a submission still reads,
// which is what the per-frame slot rotation guarantees.

// One record's kind. `RoundedRect` is the only kind slice 3 emits.
enum class RecordKind : std::uint32_t {
    RoundedRect = 0,
    // Reserved for slice 4 (`circle()`, `ellipse()`, `inset()` with elliptical radii).
    Ellipse = 1,
    // Reserved for slice 5 (`polygon()`).
    Polygon = 2,
};

// Record flags. Slice 3 sets none; the polygon fill rule lands with slice 5.
inline constexpr std::uint32_t kRecordFlagEvenOdd = 1U << 0U;

// The record as the shader reads it. `radii[0..3]` are the corner radii in TL, TR, BR, BL order;
// `radii[4..7]` are the second component of each corner's elliptical pair, zero for a circular
// corner.
struct ClipRecord {
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float radii[8] = {};

    [[nodiscard]] bool has_area() const {
        return width > 0.0f && height > 0.0f;
    }
};

// The header at the start of the buffer.
struct ClipBufferHeader {
    std::uint32_t record_count = 0;
    std::uint32_t reserved[3] = {};
};

// One interned clip set: a range into the record list. `count == 0` is the empty set, which is what
// an unclipped batch uses, so the shader's loop is a no-op there.
struct ClipSetRange {
    std::uint32_t first_record = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved[2] = {};
};

static_assert(sizeof(ClipRecord) == 64, "a clip record is 64 bytes (std430, 16-byte aligned)");
static_assert(sizeof(ClipBufferHeader) == 16, "the clip buffer header is 16 bytes");
static_assert(sizeof(ClipSetRange) == 16, "a clip set range is 16 bytes");

// The buffer's byte layout: header, then records, then the interned sets. The sets come last
// because the record list grows as clips are pushed and the sets are written once per frame at the
// end.
//
// `max_records` and `max_sets` are the capacities the caller sized the buffer for. The offsets are
// computed from them, so the shader's `sets` array base must match: the shader declares the same
// capacities as compile-time constants.
[[nodiscard]] constexpr std::size_t records_offset() {
    return sizeof(ClipBufferHeader);
}

[[nodiscard]] constexpr std::size_t sets_offset(std::uint32_t max_records) {
    return records_offset() + static_cast<std::size_t>(max_records) * sizeof(ClipRecord);
}

[[nodiscard]] constexpr std::size_t buffer_bytes(std::uint32_t max_records,
                                                 std::uint32_t max_sets) {
    return sets_offset(max_records) + static_cast<std::size_t>(max_sets) * sizeof(ClipSetRange);
}

// The capacities slice 3 sizes the buffer for. `kMaxClipRecords` bounds the record list (a frame
// that pushes more clips than this folds the overflow into the last record, the same conservative
// rule the stack already used); `kMaxClipSets` bounds the interned sets, which is one per distinct
// folded clip set in a frame.
//
// 256 records is 16 KB and 256 sets is 4 KB, so the whole buffer is 20 KB per frame in flight:
// small enough to keep the per-frame upload trivial, large enough that no realistic scene folds.
inline constexpr std::uint32_t kMaxClipRecords = 256;
inline constexpr std::uint32_t kMaxClipSets = 256;

static_assert(buffer_bytes(kMaxClipRecords, kMaxClipSets) == 16 + 256 * 64 + 256 * 16,
              "the clip buffer layout must match the documented byte count");

} // namespace tessera::rendering::gpu::clip
