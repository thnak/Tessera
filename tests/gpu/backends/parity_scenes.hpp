// SPDX-License-Identifier: MIT
//
// The fixed scene set of the cross-backend image parity harness (issue #245). Every scene is a
// small, clock-free draw list emitted through the batched 2D renderer, so what reaches the RHI is
// identical on every backend: only the rasterizer can differ. The set covers the primitives the
// issue names - a solid quad, an SDF rounded quad, textured quads, a clip, a transform, text, a
// shadow and an alpha blend - at one fixed extent so the reference images are comparable.

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::tests::gpu::backends {

inline constexpr std::uint32_t kSceneWidth = 128;
inline constexpr std::uint32_t kSceneHeight = 96;

// Textures every scene is allowed to sample. Created once per device; the pixel data is
// deterministic and tiny (no fonts, no asset files).
struct SceneTextures {
    std::unique_ptr<rendering::rhi::Texture> glyphs;  // 6 glyph cells, 8x8 each
    std::unique_ptr<rendering::rhi::Texture> checker; // 1:1 checkerboard
    std::unique_ptr<rendering::rhi::Texture> ramp;    // 3x-stretched nearest ramp
};

[[nodiscard]] SceneTextures create_scene_textures(rendering::rhi::Device& device);

struct Scene {
    std::string_view name;
    void (*draw)(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures& textures);
};

// The scene set in a fixed, sorted-by-purpose order; the report and reference directory use these
// names.
[[nodiscard]] std::span<const Scene> parity_scenes();

// The clear colour of every parity target.
[[nodiscard]] rendering::rhi::Color parity_clear_color() noexcept;

} // namespace tessera::tests::gpu::backends
