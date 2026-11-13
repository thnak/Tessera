// SPDX-License-Identifier: MIT
//
// Renders one parity scene through the RHI and reads the RGBA8 image back. Synchronous on purpose:
// this is an offscreen test harness, so submit() then copy_target_to_buffer() is the simplest
// deterministic path, and no frame code learns a new blocking call.

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::tests::gpu::backends {

struct RenderedScene {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint32_t draw_calls = 0;
    std::uint32_t vertices = 0;
};

// Throws std::runtime_error when a backend resource cannot be created; the harness turns that into
// a failed run that names the arm and scene.
[[nodiscard]] RenderedScene render_scene(rendering::rhi::Device& device,
                                         rendering::rhi::Pipeline& pipeline,
                                         std::string_view scene);

} // namespace tessera::tests::gpu::backends
