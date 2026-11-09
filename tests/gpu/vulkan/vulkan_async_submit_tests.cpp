// SPDX-License-Identifier: MIT
//
// Asynchronous submission: submit_async() must not block, completion must be observable by polling
// and bounded waits, results must match the synchronous path, and a list destroyed while the GPU
// still runs it must not corrupt later lists (its command resources are recycled only after the
// fence signals).

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Buffer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::CommandList;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;

constexpr std::uint32_t kSize = 8;
constexpr std::uint64_t kWaitMs = 5000;
constexpr int kChurnIterations = 200;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_async_submit: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc(float red) {
    RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.clear_color = {red, 0.0f, 0.0f, 1.0f};
    return desc;
}

// Records "clear the target, copy it into `readback`" without submitting.
void record_clear_and_copy(CommandList& commands, RenderTarget& target,
                           const RenderTargetDesc& desc, Buffer& readback) {
    commands.begin_render_pass(target, desc);
    commands.end_render_pass();
    commands.copy_target_to_buffer(target, readback);
}

std::uint8_t first_red(const Buffer& readback) {
    std::array<std::uint8_t, 4> pixel{};
    readback.read(pixel.data(), pixel.size());
    return pixel[0];
}

bool check_basic_async(Device& device) {
    const RenderTargetDesc desc = target_desc(1.0f);
    auto target = device.create_render_target(desc);
    auto readback = device.create_buffer({kSize * kSize * 4, BufferUsage::Readback});

    auto commands = device.create_command_list();
    if (!expect(!commands->is_complete(), "unsubmitted list reports incomplete") ||
        !expect(!commands->wait(0), "wait on an unsubmitted list returns false")) {
        return false;
    }

    record_clear_and_copy(*commands, *target, desc, *readback);
    commands->submit_async();
    if (!expect(commands->wait(kWaitMs), "submitted list completes within the bounded wait") ||
        !expect(commands->is_complete(), "is_complete is true after wait succeeded") ||
        !expect(first_red(*readback) == 255, "async result matches the clear color")) {
        return false;
    }
    return true;
}

bool check_sync_still_blocks(Device& device) {
    const RenderTargetDesc desc = target_desc(0.0f);
    auto target = device.create_render_target(desc);
    auto readback = device.create_buffer({kSize * kSize * 4, BufferUsage::Readback});

    auto commands = device.create_command_list();
    record_clear_and_copy(*commands, *target, desc, *readback);
    commands->submit();
    // The synchronous contract: complete and readable immediately on return.
    return expect(commands->is_complete(), "submit() leaves the list complete") &&
           expect(first_red(*readback) == 0, "sync result matches the clear color");
}

// Drops lists while their work may still be running and immediately asks for the next one, so a
// premature recycle would reset a pool or fence that a submission still uses. The validation
// layer reports that (the test is registered to fail on any "[vulkan]" output); the final
// readback also proves the queue executed every list in order.
bool check_drop_in_flight_and_reuse(Device& device) {
    // Each in-flight frame owns its target and readback buffer, as a real frame loop does; they
    // stay alive until the queue drains, as the async contract requires. Sharing them between
    // submissions would be a genuine hazard (synchronization validation reports it).
    struct Frame {
        std::unique_ptr<RenderTarget> target;
        std::unique_ptr<Buffer> readback;
    };
    std::vector<Frame> frames;
    frames.reserve(kChurnIterations);

    for (int i = 0; i < kChurnIterations; ++i) {
        const RenderTargetDesc desc = target_desc((i % 2 == 0) ? 1.0f : 0.0f);
        Frame frame;
        frame.target = device.create_render_target(desc);
        frame.readback = device.create_buffer({kSize * kSize * 4, BufferUsage::Readback});

        auto commands = device.create_command_list();
        record_clear_and_copy(*commands, *frame.target, desc, *frame.readback);
        commands->submit_async();
        // `commands` is destroyed here without waiting: the device keeps it in flight.
        frames.push_back(std::move(frame));
    }
    device.wait_idle();

    for (int i = 0; i < kChurnIterations; ++i) {
        const std::uint8_t expected_red = (i % 2 == 0) ? 255 : 0;
        if (!expect(first_red(*frames[static_cast<std::size_t>(i)].readback) == expected_red,
                    "every dropped list produced its own result")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_async_submit: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    try {
        if (!check_basic_async(*device)) {
            return 1;
        }
        if (!check_sync_still_blocks(*device)) {
            return 2;
        }
        if (!check_drop_in_flight_and_reuse(*device)) {
            return 3;
        }
    } catch (const std::exception& error) {
        std::cerr << "vulkan_async_submit: unexpected exception: " << error.what() << "\n";
        return 4;
    }
    return 0;
}
