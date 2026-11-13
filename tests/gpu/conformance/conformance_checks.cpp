// SPDX-License-Identifier: MIT
//
// The conformance checks themselves: one function per RHI contract area on the harness's golden
// path. Each function returns pass, skip or fail with a fixed, machine-independent message so the
// summary stays deterministic. GPU work is deliberately modest (at most 64x64) and nothing here
// reads a clock.

#include "conformance_backend.hpp"
#include "conformance_check.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace tessera::tests::gpu::conformance {
namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kTargetExtent = 64;

CheckResult device_create(rhi::Device& device) {
    // The backends this suite can select (issue #245 adds WebGPU alongside null/Vulkan; Metal is a
    // separate decision, ADR-053, and is not part of this harness).
    const rhi::Backend backend = device.backend();
    if (backend != rhi::Backend::Null && backend != rhi::Backend::Vulkan &&
        backend != rhi::Backend::WebGpu) {
        return CheckResult::fail("device reports a backend the harness cannot select");
    }
    return CheckResult::pass();
}

CheckResult buffer_roundtrip(rhi::Device& device) {
    constexpr std::size_t kSize = 64;
    const auto buffer = device.create_buffer({kSize, rhi::BufferUsage::Readback});
    if (buffer == nullptr) {
        return CheckResult::fail("create_buffer returned null");
    }
    if (buffer->size() != kSize) {
        return CheckResult::fail("buffer size does not match the descriptor");
    }

    std::array<std::uint8_t, kSize> input{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::uint8_t>(index * 7U + 3U);
    }
    buffer->write(input.data(), input.size());

    std::array<std::uint8_t, kSize> output{};
    buffer->read(output.data(), output.size());
    if (output != input) {
        return CheckResult::fail("buffer read-back does not match the written bytes");
    }

    // A zero-length range at the very end is in bounds and must stay accepted.
    buffer->write(input.data(), 0, kSize);
    return CheckResult::pass();
}

CheckResult buffer_out_of_range(rhi::Device& device) {
    constexpr std::size_t kSize = 16;
    const auto buffer = device.create_buffer({kSize, rhi::BufferUsage::Readback});
    if (buffer == nullptr) {
        return CheckResult::fail("create_buffer returned null");
    }

    std::array<std::uint8_t, kSize + 8> data{};
    data.fill(0xA5);
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    const auto throws_out_of_range = [](auto&& call) {
        try {
            call();
        } catch (const std::out_of_range&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    bool exact_fit = true;
    try {
        buffer->write(data.data(), kSize, 0);
        buffer->read(data.data(), kSize, 0);
    } catch (...) {
        exact_fit = false;
    }
    if (!exact_fit) {
        return CheckResult::fail("an in-range write/read was rejected");
    }
    if (!throws_out_of_range([&] { buffer->write(data.data(), kSize + 1, 0); })) {
        return CheckResult::fail("a write larger than the buffer did not throw std::out_of_range");
    }
    if (!throws_out_of_range([&] { buffer->write(data.data(), 8, 9); })) {
        return CheckResult::fail("a write past the end did not throw std::out_of_range");
    }
    if (!throws_out_of_range([&] { buffer->write(data.data(), 1, kSize); })) {
        return CheckResult::fail("a write starting at the end did not throw std::out_of_range");
    }
    if (!throws_out_of_range([&] { buffer->write(data.data(), 8, max - 3); })) {
        return CheckResult::fail("an overflowing write offset was not rejected");
    }
    if (!throws_out_of_range([&] { buffer->read(data.data(), kSize + 1, 0); })) {
        return CheckResult::fail("a read larger than the buffer did not throw std::out_of_range");
    }
    if (!throws_out_of_range([&] { buffer->read(data.data(), 8, 9); })) {
        return CheckResult::fail("a read past the end did not throw std::out_of_range");
    }
    if (!throws_out_of_range([&] { buffer->read(data.data(), 8, max - 3); })) {
        return CheckResult::fail("an overflowing read offset was not rejected");
    }
    return CheckResult::pass();
}

CheckResult render_target_create(rhi::Device& device) {
    rhi::RenderTargetDesc desc;
    desc.extent = {kTargetExtent, kTargetExtent};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.clear_color = rhi::rgba8(0, 0, 0, 255);
    const auto target = device.create_render_target(desc);
    if (target == nullptr) {
        return CheckResult::fail("create_render_target returned null");
    }
    if (target->extent().width != kTargetExtent || target->extent().height != kTargetExtent) {
        return CheckResult::fail("render target extent does not match the descriptor");
    }
    if (target->format() != desc.format) {
        return CheckResult::fail("render target format does not match the descriptor");
    }
    if (target->sample_count() != 1) {
        return CheckResult::fail("a 1-sample render target reports another sample count");
    }
    if (target->presentable()) {
        return CheckResult::fail("an offscreen render target reports itself as presentable");
    }
    return CheckResult::pass();
}

CheckResult texture_create(rhi::Device& device) {
    constexpr std::uint32_t kExtent = 4;
    constexpr std::size_t kBytes = kExtent * kExtent * 4;
    std::array<std::uint8_t, kBytes> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<std::uint8_t>(index * 11U + 5U);
    }

    rhi::TextureDesc desc;
    desc.extent = {kExtent, kExtent};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.pixels = pixels.data();
    desc.row_pitch = kExtent * 4;
    const auto texture = device.create_texture(desc);
    if (texture == nullptr) {
        return CheckResult::fail("create_texture returned null");
    }
    if (texture->extent().width != kExtent || texture->extent().height != kExtent) {
        return CheckResult::fail("texture extent does not match the descriptor");
    }
    if (texture->format() != desc.format) {
        return CheckResult::fail("texture format does not match the descriptor");
    }
    if (texture->sample_count() != 1) {
        return CheckResult::fail("a 1-sample texture reports another sample count");
    }
    // The dynamic-atlas update path must accept one tightly packed image of the same size.
    texture->write(pixels.data(), pixels.size());
    return CheckResult::pass();
}

CheckResult pipeline_create(rhi::Device& device) {
    const rhi::PipelineDesc desc = pipeline_template(device.backend());
    const auto pipeline = device.create_pipeline(desc);
    if (pipeline == nullptr) {
        return CheckResult::fail("create_pipeline returned null");
    }
    if (pipeline->sample_count() != desc.sample_count) {
        return CheckResult::fail("pipeline sample count does not match the descriptor");
    }
    return CheckResult::pass();
}

CheckResult command_list_record_submit(rhi::Device& device) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kTargetExtent, kTargetExtent};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = rhi::rgba8(0, 0, 0, 255);
    target_desc.load_op = rhi::LoadOp::Clear;
    target_desc.store_op = rhi::StoreOp::Store;
    const auto target = device.create_render_target(target_desc);
    const auto readback = device.create_buffer(
        {static_cast<std::size_t>(kTargetExtent) * kTargetExtent * 4, rhi::BufferUsage::Readback});
    const auto commands = device.create_command_list();
    if (target == nullptr || readback == nullptr || commands == nullptr) {
        return CheckResult::fail("one of the command-list resources could not be created");
    }

    commands->begin_render_pass(*target, target_desc);
    commands->set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(kTargetExtent),
                                         static_cast<float>(kTargetExtent), 0.0f, 1.0f});
    commands->set_scissor(rhi::Scissor{0, 0, kTargetExtent, kTargetExtent});
    commands->end_render_pass();
    commands->copy_target_to_buffer(*target, *readback);
    commands->submit();
    if (!commands->is_complete()) {
        return CheckResult::fail("the command list is not complete after submit()");
    }
    return CheckResult::pass();
}

CheckResult device_status_ready(rhi::Device& device) {
    if (device.status() != rhi::DeviceStatus::Ready) {
        return CheckResult::fail("device status is not Ready");
    }
    return CheckResult::pass();
}

CheckResult timestamp_capability_honest(rhi::Device& device) {
    const rhi::GpuTimestampCapability capability = device.timestamp_capability();
    const bool expected = capability.queries_supported && capability.queue_supported &&
                          std::isfinite(capability.period_ns) && capability.period_ns > 0.0;
    if (capability.usable() != expected) {
        return CheckResult::fail("GpuTimestampCapability::usable() disagrees with its fields");
    }

    const auto commands = device.create_command_list();
    if (commands == nullptr) {
        return CheckResult::fail("create_command_list returned null");
    }
    if (commands->begin_gpu_timestamps(0) != capability.usable()) {
        return CheckResult::fail("command-list timestamp arming disagrees with the device probe");
    }
    if (capability.usable()) {
        double frame_time_ms = 0.0;
        if (commands->gpu_frame_time_ms(frame_time_ms)) {
            return CheckResult::fail("a GPU frame time was reported before any submission");
        }
    }
    return CheckResult::pass();
}

CheckResult timestamp_query_roundtrip(rhi::Device& device) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {16, 16};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = rhi::rgba8(0, 0, 0, 255);
    const auto target = device.create_render_target(target_desc);
    const auto commands = device.create_command_list();
    if (target == nullptr || commands == nullptr) {
        return CheckResult::fail("the timestamp resources could not be created");
    }
    if (!commands->begin_gpu_timestamps(0)) {
        return CheckResult::fail("the usable device probe did not accept timestamp arming");
    }

    commands->begin_render_pass(*target, target_desc);
    commands->end_render_pass();
    commands->submit();
    if (!commands->is_complete()) {
        return CheckResult::fail("the timestamped submission did not complete");
    }
    // An equal tick pair has no measured interval; gpu_frame_time_ms() then reports false and the
    // caller keeps the CPU signal, which is the documented fallback rather than a failure.
    double frame_time_ms = 0.0;
    if (commands->gpu_frame_time_ms(frame_time_ms) &&
        !(frame_time_ms > 0.0 && frame_time_ms <= rhi::kMaxPlausibleTimestampIntervalMs)) {
        return CheckResult::fail("the reported GPU frame interval is not plausible");
    }
    return CheckResult::pass();
}

constexpr std::array<Check, 10> kChecks = {{
    {"buffer_out_of_range", Capability::BufferBounds, &buffer_out_of_range},
    {"buffer_roundtrip", Capability::BufferReadback, &buffer_roundtrip},
    {"command_list_record_submit", Capability::CommandList, &command_list_record_submit},
    {"device_create", Capability::Device, &device_create},
    {"device_status_ready", Capability::DeviceStatus, &device_status_ready},
    {"pipeline_create", Capability::Pipeline, &pipeline_create},
    {"render_target_create", Capability::RenderTarget, &render_target_create},
    {"texture_create", Capability::Texture, &texture_create},
    {"timestamp_capability_honest", Capability::Device, &timestamp_capability_honest},
    {"timestamp_query_roundtrip", Capability::GpuTimestamps, &timestamp_query_roundtrip},
}};

} // namespace

std::span<const Check> checks() {
    return kChecks;
}

} // namespace tessera::tests::gpu::conformance
