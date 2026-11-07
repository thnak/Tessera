// SPDX-License-Identifier: MIT

#pragma once

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

struct Batched2DPushConstants {
    float viewport_width = 0;
    float viewport_height = 0;
    float aa_radius = 1.0f;
    float pad = 0;
};

static_assert(sizeof(Batched2DPushConstants) == 16);

} // namespace tessera::rendering::gpu
