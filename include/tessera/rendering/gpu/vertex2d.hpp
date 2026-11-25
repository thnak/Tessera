// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"
#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {

// `alignas(64)` so a vertex never straddles a cache line (security review finding C7). The struct
// is exactly 64 bytes, but its natural alignment is 4, so a `std::vector<Vertex2D>` base is only
// 16-byte aligned and three of four vertices span two lines. The vertex stream is the hottest
// per-frame buffer in the renderer, so that doubles its store traffic. The alignment changes no
// offset, so the attribute table and the stride below are unaffected.
TESSERA_CACHE_LINE_BEGIN
struct alignas(64) Vertex2D {
    float x = 0;
    float y = 0;
    float u = 0;
    float v = 0;
    std::uint32_t color = 0xFFFFFFFFu;
    float local_x = 0;
    float local_y = 0;
    float half_width = 0;
    float half_height = 0;
    float radius = 0;
    float mode = 0;
    // The carved-out box of a box shadow (issue #461): xy = its centre relative to the quad centre
    // (local px), zw = its half size. A shadow is painted only outside this box (CSS Backgrounds 3
    // section 7.1.1 clips an outer shadow inside the border box), and its corner radius is read
    // back from `radius` minus the box's inset. Zero (the default) subtracts nothing, so every
    // other primitive keeps the 44-byte prefix it always had and the shader's carve branch is
    // skipped.
    float carve_x = 0;
    float carve_y = 0;
    float carve_half_width = 0;
    float carve_half_height = 0;
    // Index into the batch's sampled-image array (RHI 1.7, issue #480 part 2). 0 on the
    // one-set-per-texture path, where the shader reads the single bound image and ignores this; on
    // the array path it is the batch's slot, so consecutive quads over different textures share one
    // draw. It rides in the vertex stream for the same reason the carve box does: a per-draw value
    // would split the batch it is meant to keep together.
    std::uint32_t texture_index = 0;
};
TESSERA_CACHE_LINE_END

// 44 bytes of long-standing layout plus the 16-byte carve box the soft-shadow fragment stage needs,
// plus the 4-byte texture-array index (RHI 1.7, issue #480 part 2). The carve is per primitive, so
// it rides in the vertex stream: putting it in the push-constant block would make it per draw and
// split the batch around every shadow (the batch key is texture, scissor and folded clip state),
// and the 112-byte block has no room left inside Vulkan's 128-byte minimum. See
// docs/design/gpu/box-shadow-clipping.md.
static_assert(sizeof(Vertex2D) == 64, "Vertex2D layout must stay stable");
static_assert(alignof(Vertex2D) == 64, "a vertex must not straddle a cache line");

// `Vertex2D::mode`: 0 solid, 1 rounded rect, 2 textured, and 3 + w for a rounded-rect outline of
// stroke width w (the shaders read the width back as mode - 3).
inline constexpr float kRingModeBase = 3.0f;

// A textured quad is mode 2. A glyph quad may add `(boost - 1) / kCoverageBoostScale` to it (so
// mode stays in [2, 2.5)): the shader multiplies the sampled alpha by `boost`, clamped to 1. Light
// text on a dark surface needs it; a browser's rasterizer applies contrast to glyph coverage, and
// the raw FreeType coverage reads thinner (about 25 % less ink on white-on-blue labels).
inline constexpr float kCoverageBoostScale = 4.0f;
// A soft (Gaussian) shadow is mode kSoftShadowModeBase + sigma; the quad is the shadow box grown by
// kSoftShadowReach * sigma on every side, which the shader subtracts again to find the box. sigma 0
// is the hard-edged shadow (`box-shadow: 4px 4px 0`): the same quad and the same carve test, with a
// coverage step instead of the Gaussian.
inline constexpr float kSoftShadowModeBase = 1024.0f;
inline constexpr float kSoftShadowReach = 2.0f;
// An inset shadow (issue #612) is the same quad and the same Gaussian, with the coverage inverted:
// the shadow fills the ring between the border box and the inner box and fades inward, instead of
// painting outside the border box. The mode is kInsetShadowModeBase + sigma, so the two shadow
// kinds share one vertex layout and one fragment branch.
inline constexpr float kInsetShadowModeBase = 2048.0f;
// An exact-nearest textured quad (#682) is mode kExactNearestMode. The fragment shader maps its
// device pixel to a texel with integer math, `src = min(((2 * p + 1) * src_size) / (2 * dst_size),
// src_size - 1)`, and fetches it without a sampler. The hardware nearest sampler is not exact: at a
// 2:1 downscale every sample lands on a texel boundary and the interpolated UV picks the wrong
// neighbor (measured on Vulkan and WebGPU, tests/gpu/batched2d/cpu_to_gpu_4k_measure.cpp). The
// quad's integer device rectangle rides in the carve fields: x, y = origin and z, w = size in
// target pixels. The texture must be sampled whole (uv 0..1).
inline constexpr float kExactNearestMode = 4096.0f;

inline constexpr rhi::VertexAttribute kVertex2DAttributes[] = {
    {0, static_cast<std::uint32_t>(offsetof(Vertex2D, x)), rhi::VertexFormat::Float2},
    {1, static_cast<std::uint32_t>(offsetof(Vertex2D, u)), rhi::VertexFormat::Float2},
    {2, static_cast<std::uint32_t>(offsetof(Vertex2D, color)), rhi::VertexFormat::UByte4Norm},
    {3, static_cast<std::uint32_t>(offsetof(Vertex2D, local_x)), rhi::VertexFormat::Float2},
    {4, static_cast<std::uint32_t>(offsetof(Vertex2D, half_width)), rhi::VertexFormat::Float2},
    {5, static_cast<std::uint32_t>(offsetof(Vertex2D, radius)), rhi::VertexFormat::Float},
    {6, static_cast<std::uint32_t>(offsetof(Vertex2D, mode)), rhi::VertexFormat::Float},
    {7, static_cast<std::uint32_t>(offsetof(Vertex2D, carve_x)), rhi::VertexFormat::Float4},
    {8, static_cast<std::uint32_t>(offsetof(Vertex2D, texture_index)), rhi::VertexFormat::UInt},
};

inline constexpr std::uint32_t kVertex2DAttributeCount = 9;
inline constexpr std::uint32_t kVertex2DStride = sizeof(Vertex2D);

// Push-constant block shared by the batched2d vertex and fragment stages.
//
// Since ADR-068 slice 3 the clip set is no longer in here: the fragment shader reads a per-frame
// clip-record buffer (set 1, `clip_records.hpp`) and this block carries only the interned clip-set
// index. That is what removed the three-clip ceiling and the fixed 32-byte slot per clip, and it
// shrank the block from 112 bytes to 16.
struct Batched2DPushConstants {
    float viewport_width = 0;
    float viewport_height = 0;
    float aa_radius = 1.0f;
    // Index into the clip-record buffer's interned set list. 0 is always the empty set, so an
    // unclipped batch needs no special case and the shader's loop is a no-op.
    std::uint32_t clip_set = 0;
};

static_assert(sizeof(Batched2DPushConstants) == 16,
              "push constants must fit the 128-byte Vulkan minimum");

} // namespace tessera::rendering::gpu
