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
};

static_assert(sizeof(Vertex2D) == 44, "Vertex2D layout must stay stable");

inline constexpr rhi::VertexAttribute kVertex2DAttributes[] = {
    {0, static_cast<std::uint32_t>(offsetof(Vertex2D, x)), rhi::VertexFormat::Float2},
    {1, static_cast<std::uint32_t>(offsetof(Vertex2D, u)), rhi::VertexFormat::Float2},
    {2, static_cast<std::uint32_t>(offsetof(Vertex2D, color)), rhi::VertexFormat::UByte4Norm},
    {3, static_cast<std::uint32_t>(offsetof(Vertex2D, local_x)), rhi::VertexFormat::Float2},
    {4, static_cast<std::uint32_t>(offsetof(Vertex2D, half_width)), rhi::VertexFormat::Float2},
    {5, static_cast<std::uint32_t>(offsetof(Vertex2D, radius)), rhi::VertexFormat::Float},
    {6, static_cast<std::uint32_t>(offsetof(Vertex2D, mode)), rhi::VertexFormat::Float},
};

inline constexpr std::uint32_t kVertex2DAttributeCount = 7;
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
