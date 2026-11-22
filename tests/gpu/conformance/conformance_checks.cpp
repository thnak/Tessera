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
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tessera::tests::gpu::conformance {
namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kTargetExtent = 64;

CheckResult device_create(rhi::Device& device) {
    // The backends this suite can select (issue #245 adds WebGPU alongside null/Vulkan; Metal is
    // ADR-053 and joins the harness with its own arm).
    const rhi::Backend backend = device.backend();
    if (backend != rhi::Backend::Null && backend != rhi::Backend::Vulkan &&
        backend != rhi::Backend::WebGpu && backend != rhi::Backend::Metal) {
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

// RHI 1.9 `Texture::write_region`: every backend accepts the same regions and refuses the same
// ones. Pixel content is checked by the backends' own readback tests (texture_write_region on
// Vulkan, the unit test on the null device); here the contract is acceptance and refusal.
CheckResult texture_write_region(rhi::Device& device) {
    constexpr std::uint32_t kExtent = 8;
    std::array<std::uint8_t, kExtent * kExtent * 4> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<std::uint8_t>(index * 7U + 3U);
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
    std::array<std::uint8_t, kExtent * kExtent * 4> source{};
    const auto throws = [](auto&& body, bool out_of_range) {
        try {
            body();
        } catch (const std::out_of_range&) {
            return out_of_range;
        } catch (const std::invalid_argument&) {
            return !out_of_range;
        }
        return false;
    };

    // Tightly packed, pitched (rows inside a wider source) and flush with the far corner.
    texture->write_region({1, 2, 3, 4}, source.data(), 0);
    texture->write_region({1, 2, 3, 4}, source.data(), kExtent * 4);
    texture->write_region({kExtent - 2, kExtent - 3, 2, 3}, source.data(), kExtent * 4);
    texture->write_region({0, 0, kExtent, kExtent}, source.data(), 0);
    // An empty region is valid and writes nothing, even with no data.
    texture->write_region({3, 3, 0, 5}, nullptr, 0);
    texture->write_region({3, 3, 5, 0}, nullptr, 0);

    if (!throws([&] { texture->write_region({kExtent - 1, 0, 2, 1}, source.data(), 0); }, true)) {
        return CheckResult::fail("a region past the right edge is not refused");
    }
    if (!throws([&] { texture->write_region({0, kExtent - 1, 1, 2}, source.data(), 0); }, true)) {
        return CheckResult::fail("a region past the bottom edge is not refused");
    }
    if (!throws([&] { texture->write_region({kExtent + 1, 0, 1, 1}, source.data(), 0); }, true)) {
        return CheckResult::fail("a region starting outside the texture is not refused");
    }
    if (!throws([&] { texture->write_region({0, 0, 3, 1}, source.data(), 3 * 4 - 1); }, false)) {
        return CheckResult::fail("a row pitch shorter than one region row is not refused");
    }
    if (!throws([&] { texture->write_region({0, 0, 1, 1}, nullptr, 0); }, false)) {
        return CheckResult::fail("a non-empty region without data is not refused");
    }
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

// RHI 1.12 `CommandList::bind_storage_buffer` (ADR-068 slice 1): the contract is that a read-only
// storage buffer reaches the fragment shader. The null backend records the bind; a real backend is
// proven by reading the buffer's contents back out of the rendered target, which is the only
// evidence that the descriptor reached the shader rather than merely being accepted.
//
// The check is capability-gated: a backend answering `supports_storage_buffers() == false` reports
// a skip naming the capability, never a pass.
CheckResult storage_buffer_bind(rhi::Device& device) {
    if (!device.supports_storage_buffers()) {
        return CheckResult::skip("the device reports no storage buffers");
    }

    // The probe buffer holds one vec4: the colour the fragment stage writes. std430 lays a vec4 at
    // offset 0, so the bytes are the four floats in order.
    constexpr std::array<float, 4> kColor = {0.25f, 0.5f, 0.75f, 1.0f};
    const auto buffer = device.create_buffer({sizeof(kColor), rhi::BufferUsage::Storage});
    if (buffer == nullptr) {
        return CheckResult::fail("create_buffer(BufferUsage::Storage) returned null");
    }
    buffer->write(kColor.data(), sizeof(kColor));

    // A storage buffer is a shader resource, so the check needs a pipeline whose layout declares
    // it. The null backend records the descriptor; a real backend needs its own probe program,
    // which the backend-specific arm below supplies.
    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.storage_buffer = true;
    pipeline_desc.color_format = rhi::TextureFormat::R8G8B8A8Unorm;
    pipeline_desc.blend = rhi::BlendMode::None;
    if (!fill_storage_probe_pipeline(device.backend(), pipeline_desc)) {
        return CheckResult::skip("this backend has no storage-buffer probe program");
    }

    std::unique_ptr<rhi::Pipeline> pipeline;
    try {
        pipeline = device.create_pipeline(pipeline_desc);
    } catch (const std::exception& error) {
        return CheckResult::fail(std::string("create_pipeline(storage_buffer) threw: ") +
                                 error.what());
    }
    if (pipeline == nullptr) {
        return CheckResult::fail("create_pipeline(storage_buffer) returned null");
    }

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kTargetExtent, kTargetExtent};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = rhi::rgba8(0, 0, 0, 255);
    target_desc.load_op = rhi::LoadOp::Clear;
    target_desc.store_op = rhi::StoreOp::Store;
    const auto target = device.create_render_target(target_desc);
    const auto commands = device.create_command_list();
    if (target == nullptr || commands == nullptr) {
        return CheckResult::fail("the storage-buffer resources could not be created");
    }

    // The misuse the contract names: a zero-size buffer is unrepresentable (Vulkan cannot create
    // one), so creation refuses it rather than letting each backend fail differently.
    bool empty_refused = false;
    try {
        (void)device.create_buffer({0, rhi::BufferUsage::Storage});
    } catch (const std::invalid_argument&) {
        empty_refused = true;
    }
    if (!empty_refused) {
        return CheckResult::fail("creating a zero-size buffer was not refused");
    }

    commands->begin_render_pass(*target, target_desc);
    commands->bind_pipeline(*pipeline);
    commands->bind_storage_buffer(0, *buffer);
    commands->set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(kTargetExtent),
                                         static_cast<float>(kTargetExtent), 0.0f, 1.0f});
    commands->set_scissor(rhi::Scissor{0, 0, kTargetExtent, kTargetExtent});
    commands->draw(3);
    commands->end_render_pass();

    // Read the target back and require the buffer's colour. The null backend moves no pixels, so it
    // stops at the structural half; a real backend must produce the value.
    if (device.backend() == rhi::Backend::Null) {
        commands->submit();
        if (!commands->is_complete()) {
            return CheckResult::fail("the storage-buffer submission did not complete");
        }
        return CheckResult::pass();
    }

    const std::size_t bytes =
        static_cast<std::size_t>(kTargetExtent) * kTargetExtent * sizeof(std::uint32_t);
    const auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    if (readback == nullptr) {
        return CheckResult::fail("the storage-buffer readback buffer could not be created");
    }
    commands->copy_target_to_buffer(*target, *readback);
    commands->submit();
    if (!commands->is_complete()) {
        return CheckResult::fail("the storage-buffer submission did not complete");
    }

    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    // The centre pixel is inside the fullscreen triangle on every backend.
    const std::size_t centre =
        (static_cast<std::size_t>(kTargetExtent / 2) * kTargetExtent + kTargetExtent / 2) * 4;
    const auto expected = [](float value) {
        return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    };
    const std::array<std::uint8_t, 3> want = {expected(kColor[0]), expected(kColor[1]),
                                              expected(kColor[2])};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int actual = pixels[centre + channel];
        const int difference = actual > want[channel] ? actual - want[channel]
                                                      : static_cast<int>(want[channel]) - actual;
        // The same 1 LSB tolerance the conformance suite uses for every backend comparison.
        if (difference > 1) {
            return CheckResult::fail("the rendered pixel does not carry the storage buffer's "
                                     "colour (channel " +
                                     std::to_string(channel) + ": got " + std::to_string(actual) +
                                     ", want " + std::to_string(want[channel]) + ")");
        }
    }
    return CheckResult::pass();
}

constexpr std::array<Check, 12> kChecks = {{
    {"buffer_out_of_range", Capability::BufferBounds, &buffer_out_of_range},
    {"buffer_roundtrip", Capability::BufferReadback, &buffer_roundtrip},
    {"command_list_record_submit", Capability::CommandList, &command_list_record_submit},
    {"device_create", Capability::Device, &device_create},
    {"device_status_ready", Capability::DeviceStatus, &device_status_ready},
    {"pipeline_create", Capability::Pipeline, &pipeline_create},
    {"render_target_create", Capability::RenderTarget, &render_target_create},
    {"storage_buffer_bind", Capability::StorageBuffer, &storage_buffer_bind},
    {"texture_create", Capability::Texture, &texture_create},
    {"texture_write_region", Capability::Texture, &texture_write_region},
    {"timestamp_capability_honest", Capability::Device, &timestamp_capability_honest},
    {"timestamp_query_roundtrip", Capability::GpuTimestamps, &timestamp_query_roundtrip},
}};

} // namespace

std::span<const Check> checks() {
    return kChecks;
}

} // namespace tessera::tests::gpu::conformance
