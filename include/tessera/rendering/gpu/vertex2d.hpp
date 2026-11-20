// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {

struct Vertex2D {
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

// 44 bytes of long-standing layout plus the 16-byte carve box the soft-shadow fragment stage needs,
// plus the 4-byte texture-array index (RHI 1.7, issue #480 part 2). The carve is per primitive, so
// it rides in the vertex stream: putting it in the push-constant block would make it per draw and
// split the batch around every shadow (the batch key is texture, scissor and folded clip state),
// and the 112-byte block has no room left inside Vulkan's 128-byte minimum. See
// docs/design/gpu/box-shadow-clipping.md.
static_assert(sizeof(Vertex2D) == 64, "Vertex2D layout must stay stable");

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

// Push-constant block shared by the batched2d vertex and fragment stages. `clips` carries the
// folded rounded clips for the current batch: the fragment shader discards outside each one. The
// block stays inside Vulkan's 128-byte minimum push-constant budget (16 + 3 * 32 = 112 bytes).
struct Batched2DPushConstants {
    float viewport_width = 0;
    float viewport_height = 0;
    float aa_radius = 1.0f;
    std::uint32_t clip_count = 0;

    struct ClipUniform {
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
        float top_left = 0;
        float top_right = 0;
        float bottom_right = 0;
        float bottom_left = 0;
    };

    ClipUniform clips[clip::kMaxShaderClips]{};
};

static_assert(sizeof(Batched2DPushConstants) == 112,
              "push constants must fit the 128-byte Vulkan minimum");

} // namespace tessera::rendering::gpu
