// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace tessera::test {

// Renders a tiny frame into the acquired swapchain image. The renderer's end() transitions the
// image to PRESENT_SRC. The frame is submitted before this returns and the caller presents right
// after: blocking by default, or queued without waiting when `async` is set (the real frame-loop
// shape, where present must wait for the GPU through the swapchain image's semaphore).
//
// With `async`, `in_flight` (if given) receives the submitted list so the caller can poll or wait
// for it; otherwise the list is dropped while the GPU may still run it, which is allowed.
inline bool
render_swapchain_frame(rendering::rhi::Device& device, rendering::gpu::Batched2DRenderer& renderer,
                       rendering::rhi::Swapchain& swapchain, std::uint32_t image,
                       bool async = false,
                       std::unique_ptr<rendering::rhi::CommandList>* in_flight = nullptr) {
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
    if (async) {
        commands->submit_async();
        if (in_flight != nullptr) {
            *in_flight = std::move(commands);
        }
    } else {
        commands->submit();
    }
    return true;
}

} // namespace tessera::test
