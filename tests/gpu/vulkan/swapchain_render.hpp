// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"

#include <cstdint>

namespace tessera::test {

// Renders a tiny frame into the acquired swapchain image. The renderer's end() transitions the
// image to PRESENT_SRC; the caller submits and presents immediately after this returns.
inline bool render_swapchain_frame(rendering::rhi::Device& device,
                                   rendering::gpu::Batched2DRenderer& renderer,
                                   rendering::rhi::Swapchain& swapchain, std::uint32_t image) {
    auto* target = swapchain.image_target(image);
    if (target == nullptr || !target->presentable()) {
        return false;
    }

    rendering::rhi::RenderTargetDesc desc;
    desc.extent = swapchain.extent();
    desc.format = swapchain.format();
    desc.load_op = rendering::rhi::LoadOp::Clear;
    desc.clear_color = rendering::rhi::rgba8(17, 24, 39);

    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, desc);
    renderer.push_rect(
        {0, 0, static_cast<float>(desc.extent.width), static_cast<float>(desc.extent.height)},
        rendering::rhi::rgba8(17, 24, 39));
    renderer.push_rect({8, 8, 48, 32}, rendering::rhi::rgba8(56, 189, 248));
    renderer.push_rect({64, 8, 48, 32}, rendering::rhi::rgba8(251, 146, 60));
    renderer.end();
    commands->submit();
    return true;
}

} // namespace tessera::test
