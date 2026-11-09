// SPDX-License-Identifier: MIT
//
// Real-GPU smoke for D7 runtime orchestration: RuntimeLoop + DeviceRecoveryController drive a
// simulated Vulkan device loss through WaitingForCaller -> recovery -> one full frame. Skips when
// there is no Vulkan device or the test hooks are disabled.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/runtime_loop.hpp"

#if defined(TESSERA_VULKAN_TEST_HOOKS)
#include "vulkan_internal.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

constexpr tessera::rendering::rhi::Extent2D kExtent{4, 4};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_device_recovery_loop: " << message << "\n";
    }
    return condition;
}

#if defined(TESSERA_VULKAN_TEST_HOOKS)

// Draws a fresh frame after recovery into an offscreen target and checks the readback, proving the
// recreated device/resources actually render.
bool render_probe(tessera::rendering::vulkan::VulkanDevice& device) {
    using tessera::rendering::rhi::BufferUsage;
    using tessera::rendering::rhi::LoadOp;
    using tessera::rendering::rhi::RenderTargetDesc;
    using tessera::rendering::rhi::rgba8;
    using tessera::rendering::rhi::StoreOp;

    RenderTargetDesc target_desc;
    target_desc.extent = kExtent;
    target_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    target_desc.load_op = LoadOp::Clear;
    target_desc.store_op = StoreOp::Store;
    auto target = device.create_render_target(target_desc);
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    tessera::rendering::rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    tessera::rendering::gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_rect(
        {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)},
        rgba8(30, 60, 90));
    renderer.end();
    commands->submit();

    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto copy_commands = device.create_command_list();
    copy_commands->copy_target_to_buffer(*target, *readback);
    copy_commands->submit();

    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    const std::array<std::uint8_t, 4> expected = {30, 60, 90, 255};
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (pixels[i + channel] != expected[channel]) {
                return false;
            }
        }
    }
    return true;
}

#endif // TESSERA_VULKAN_TEST_HOOKS

} // namespace

int main() {
#if !defined(TESSERA_VULKAN_TEST_HOOKS)
    std::cout << "vulkan_device_recovery_loop: skipped (TESSERA_VULKAN_TEST_HOOKS disabled)\n";
    return 0;
#else
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::BufferUsage;
    using tessera::rendering::rhi::DeviceDesc;
    using tessera::rendering::rhi::DeviceStatus;
    using tessera::rendering::vulkan::VulkanDevice;
    using tessera::runtime::DeviceRecoveryController;
    using tessera::runtime::DeviceRecoveryState;
    using tessera::runtime::FixedStepConfig;
    using tessera::runtime::ManualClock;
    using tessera::runtime::RuntimeLoop;
    using tessera::runtime::RuntimeLoopConfig;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<VulkanDevice> device;
    try {
        device = std::make_unique<VulkanDevice>(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_device_recovery_loop: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }

    DeviceRecoveryController controller(*device);
    ManualClock clock;
    RuntimeLoopConfig config;
    config.fixed_step = FixedStepConfig{60.0, 250.0, 5, 1.0};
    config.render_on_demand = true;
    config.device_recovery = &controller;
    RuntimeLoop loop(clock, config);

    bool ok = true;
    loop.request_frame();
    auto tick = loop.tick();
    ok = expect(tick.should_render && !tick.device_blocked, "initial frame renders") && ok;

    // Keep one lost-generation RHI object alive so recovery is refused until the caller releases
    // it (the same contract the frame loop documents).
    auto stale = device->create_buffer({64, BufferUsage::Vertex});
    device->simulate_device_loss_for_testing();
    tick = loop.tick();
    ok = expect(tick.device_blocked && !tick.should_render && tick.idle,
                "lost device stops frames and idles") &&
         ok;
    ok = expect(tick.device_state == DeviceRecoveryState::WaitingForCaller,
                "outstanding resource -> WaitingForCaller") &&
         ok;

    stale.reset();
    tick = loop.tick();
    ok = expect(tick.device_recovered && tick.full_frame && tick.device_generation == 1,
                "caller release lets the controller recover") &&
         ok;
    tick = loop.tick();
    ok = expect(tick.should_render && tick.full_frame, "one full frame after recovery") && ok;
    ok = render_probe(*device) && ok;

    if (!ok) {
        std::cerr << "vulkan_device_recovery_loop: failed\n";
        return 1;
    }
    std::cout << "vulkan_device_recovery_loop: ok\n";
    return 0;
#endif
}
