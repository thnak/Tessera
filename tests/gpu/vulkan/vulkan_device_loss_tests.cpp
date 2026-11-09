// SPDX-License-Identifier: MIT
//
// D7 device-loss contract on the Vulkan backend, using the test-only fault injection seam so no
// real GPU reset is needed: a loss is detected as state (never an exception/indefinite wait),
// recovery is refused while caller resources from the lost generation are alive, a successful
// one-shot recreation restores rendering, and the failure path reports a value and keeps the Lost
// state consistent. Skips cleanly when no Vulkan device is available or hooks are disabled.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#if defined(TESSERA_VULKAN_TEST_HOOKS)
#include "vulkan_internal.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::DeviceStatus;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;

constexpr Extent2D kExtent{4, 4};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_device_loss: " << message << "\n";
    }
    return condition;
}

#if defined(TESSERA_VULKAN_TEST_HOOKS)

// Renders one opaque quad into a fresh offscreen target and verifies the readback. Proves the
// device (and the resources recreated after recovery) still renders frames.
bool render_probe_frame(tessera::rendering::vulkan::VulkanDevice& device) {
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

    Batched2DRenderer renderer(device, *pipeline, *white);
    const auto color = rgba8(20, 40, 60);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_rect(
        {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)}, color);
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
    const std::array<std::uint8_t, 4> expected = {20, 40, 60, 255};
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
    std::cout << "vulkan_device_loss: skipped (TESSERA_VULKAN_TEST_HOOKS disabled)\n";
    return 0;
#else
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;
    using tessera::rendering::rhi::DeviceRecoveryError;
    using tessera::rendering::vulkan::InjectedDeviceFault;
    using tessera::rendering::vulkan::VulkanDevice;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<VulkanDevice> device;
    try {
        device = std::make_unique<VulkanDevice>(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_device_loss: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    bool ok = expect(device->status() == DeviceStatus::Ready, "new device is Ready") &&
              expect(device->recover().error() == DeviceRecoveryError::NotLost,
                     "healthy device reports NotLost") &&
              render_probe_frame(*device);

    // 1) A loss reported by submit is state, not an exception: submit returns, the bounded wait
    // reports incomplete, and recovery recreates the device.
    {
        RenderTargetDesc target_desc;
        target_desc.extent = kExtent;
        auto target = device->create_render_target(target_desc);
        auto commands = device->create_command_list();
        commands->begin_render_pass(*target, target_desc);
        commands->end_render_pass();
        device->inject_fault_for_testing(InjectedDeviceFault::DeviceLostOnSubmit);
        commands->submit_async();
        ok = expect(device->status() == DeviceStatus::Lost, "submit loss is detected") && ok;
        ok = expect(!commands->wait(0), "wait is bounded and reports incomplete after loss") && ok;
        commands.reset();
        target.reset();
        const auto recovered = device->recover();
        ok = expect(recovered.has_value() && device->status() == DeviceStatus::Ready,
                    "recovery after submit loss succeeds") &&
             ok;
        ok = render_probe_frame(*device) && ok;
    }

    // 2) RHI objects from the lost generation invalidate; recovery is refused until they are
    // destroyed, then succeeds (the refused attempt does not consume the one-shot budget).
    {
        auto stale = device->create_buffer({64, BufferUsage::Vertex});
        device->simulate_device_loss_for_testing();
        ok = expect(device->status() == DeviceStatus::Lost, "simulated loss is surfaced") && ok;
        bool rejected_create = false;
        try {
            (void)device->create_buffer({64, BufferUsage::Vertex});
        } catch (const std::exception&) {
            rejected_create = true;
        }
        ok = expect(rejected_create, "resource creation is refused while Lost") && ok;
        const auto blocked = device->recover();
        ok = expect(!blocked && blocked.error() == DeviceRecoveryError::ResourcesOutstanding,
                    "recovery refuses while lost-generation resources are alive") &&
             ok;
        stale.reset();
        const auto recovered = device->recover();
        ok = expect(recovered.has_value() && device->status() == DeviceStatus::Ready,
                    "recovery succeeds after releasing resources") &&
             ok;
        ok = render_probe_frame(*device) && ok;
    }

    // 3) Failure path: the error is a value, the device stays Lost, and the one-shot policy blocks
    // a silent retry. The device is then destroyed from the Lost state without crashing.
    {
        device->simulate_device_loss_for_testing();
        device->fail_next_recovery_for_testing();
        const auto failed = device->recover();
        ok = expect(!failed && failed.error() == DeviceRecoveryError::DeviceCreationFailed,
                    "failed recovery reports DeviceCreationFailed") &&
             ok;
        ok = expect(device->status() == DeviceStatus::Lost, "failed recovery keeps Lost state") &&
             ok;
        const auto second = device->recover();
        ok = expect(!second && second.error() == DeviceRecoveryError::AlreadyAttempted,
                    "one-shot recovery policy rejects a second attempt") &&
             ok;
    }

    device.reset();
    if (!ok) {
        std::cerr << "vulkan_device_loss: failed\n";
        return 1;
    }
    std::cout << "vulkan_device_loss: ok\n";
    return 0;
#endif
}
