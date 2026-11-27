// SPDX-License-Identifier: MIT
//
// Regression test for the Vulkan barrier source scope of a recycled offscreen target
// (issue #617, docs/design/gpu/offscreen-layers.md section 5e, "Backend ordering").
//
// A pooled target that a pass sampled in a fragment shader is released and cleared again inside the
// same command list. That second pass writes the image the first pass read: a write-after-read
// hazard that needs an execution dependency on the fragment-shader stage. `begin_render_pass` used
// TOP_OF_PIPE as the barrier's source scope for such a pass, which orders nothing after the read,
// so synchronization validation reports a hazard (and a real GPU may overwrite the target while the
// earlier draw still reads it). The fix takes the source scope from the target's tracked layout.
//
// The sequence recorded here is the smallest one that exercises the path, with no renderer between:
//   1. a pass clears target A;
//   2. A is bound as a sampled target and a fullscreen draw into B reads it;
//   3. a pass clears A again (the recycle);
//   4. A is copied out with a region copy and cleared again, twice (a transfer read, then the
//   recycle).
// Evidence: the Khronos validation layer with synchronization validation reports nothing, and A
// holds the second clear colour afterwards. With the fix reverted (the source scope back to
// TOP_OF_PIPE), step 4 reports a WRITE_AFTER_READ hazard ("image layout transition ... previously
// read by vkCmdCopyImage") and this test fails on the message count. Step 3 is the sampled case:
// the validation layer accepts it with or without the fix because the barrier that ends the reading
// pass already supplies the execution dependency, so it pins the sampled-bind release and the
// pixels, not the scope.
//
// The test needs the validation layer: without it the hazard cannot be observed, so it skips with a
// reason rather than passing vacuously. It also skips when no Vulkan device is available.

#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;

constexpr std::uint32_t kSize = 32;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_recycle_barrier_gpu: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc(const rhi::Color& clear) {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = clear;
    return desc;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "layer_recycle_barrier_gpu: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }
    if (!tessera::rendering::vulkan::validation_active()) {
        std::cout << "layer_recycle_barrier_gpu: skipped (validation layer unavailable; the "
                     "hazard cannot be observed without it)\n";
        return 0;
    }
    tessera::rendering::vulkan::reset_validation_message_count();

    const rhi::Color first_clear = rhi::rgba8(200, 30, 30);
    const rhi::Color second_clear = rhi::rgba8(20, 60, 220);
    auto target_a = device->create_render_target(target_desc(first_clear));
    auto target_b = device->create_render_target(target_desc(rhi::rgba8(0, 0, 0)));
    auto glass = device->create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc());

    {
        auto commands = device->create_command_list();
        // 1. A holds content.
        commands->begin_render_pass(*target_a, target_desc(first_clear));
        commands->end_render_pass();

        // 2. A is sampled by a fullscreen draw into B (the composite/blur/mask read of a layer).
        const tessera::rendering::gpu::GlassPushConstants constants;
        commands->bind_pipeline(*glass);
        commands->bind_sampled_target(0, *target_a);
        commands->begin_render_pass(*target_b, target_desc(rhi::rgba8(0, 0, 0)));
        commands->set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
        commands->set_scissor(rhi::Scissor{0, 0, kSize, kSize});
        commands->push_constants(&constants, sizeof(constants));
        commands->draw(3);
        commands->end_render_pass();

        // 3. The pool recycles A inside the same list: cleared again after the read above.
        commands->begin_render_pass(*target_a, target_desc(second_clear));
        commands->end_render_pass();

        // The same recycle after a transfer read: a region copy out of A (a glass backdrop or a
        // layer snapshot) is not inside a pass, so no pass-end barrier orders it before the write.
        commands->copy_target_region(*target_a, rhi::Scissor{0, 0, kSize, kSize}, *target_b,
                                     rhi::Offset2D{});
        commands->begin_render_pass(*target_a, target_desc(first_clear));
        commands->end_render_pass();
        commands->begin_render_pass(*target_a, target_desc(second_clear));
        commands->end_render_pass();
        commands->submit();
    }
    device->wait_idle();

    // Read A back: the recycle's clear landed.
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device->create_buffer({bytes, rhi::BufferUsage::Readback});
    {
        auto commands = device->create_command_list();
        commands->copy_target_to_buffer(*target_a, *readback);
        commands->submit();
    }
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());

    const std::uint64_t messages = tessera::rendering::vulkan::validation_message_count();
    bool ok = expect(pixels[0] == 20 && pixels[1] == 60 && pixels[2] == 220,
                     "the recycled target holds the second clear colour");
    ok =
        expect(messages == 0,
               "recycling a sampled or copied-from target inside a list reported no validation "
               "message (a barrier source scope that orders nothing after the read is a hazard)") &&
        ok;
    if (!ok) {
        std::cerr << "layer_recycle_barrier_gpu: failed (validation=" << messages << ")\n";
        return 1;
    }
    std::cout << "layer_recycle_barrier_gpu: ok\n";
    return 0;
}
