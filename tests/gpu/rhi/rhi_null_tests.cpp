// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace tessera::rendering;

    auto device = null::create_null_device(rhi::DeviceDesc{});
    if (device->backend() != rhi::Backend::Null) {
        std::cerr << "unexpected backend\n";
        return 1;
    }

    auto buffer = device->create_buffer(rhi::BufferDesc{64, rhi::BufferUsage::Vertex});
    const std::uint32_t input[4] = {1, 2, 3, 4};
    buffer->write(input, sizeof(input));
    std::uint32_t output[4] = {};
    buffer->read(output, sizeof(output));
    if (std::memcmp(input, output, sizeof(input)) != 0) {
        std::cerr << "buffer round-trip failed\n";
        return 2;
    }

    auto target = device->create_render_target(rhi::RenderTargetDesc{{64, 64}});
    auto texture = device->create_texture(rhi::TextureDesc{{1, 1}});
    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto commands = device->create_command_list();

    commands->begin_render_pass(*target, rhi::RenderTargetDesc{{64, 64}});
    commands->bind_pipeline(*pipeline);
    commands->bind_vertex_buffer(*buffer);
    commands->bind_texture(0, *texture);
    commands->set_scissor(rhi::Scissor{0, 0, 64, 64});
    commands->draw(6);
    commands->end_render_pass();
    commands->copy_target_to_buffer(*target, *buffer);
    commands->submit();

    auto* null_commands = dynamic_cast<null::NullCommandList*>(commands.get());
    if (null_commands == nullptr) {
        std::cerr << "unexpected command list type\n";
        return 3;
    }

    const null::NullCommandStats& stats = null_commands->stats();
    if (stats.begin_render_passes != 1 || stats.end_render_passes != 1) {
        std::cerr << "render pass stats mismatch\n";
        return 4;
    }
    if (stats.pipeline_binds != 1 || stats.vertex_buffer_binds != 1 || stats.texture_binds != 1) {
        std::cerr << "bind stats mismatch\n";
        return 5;
    }
    if (stats.draws != 1 || stats.vertices != 6) {
        std::cerr << "draw stats mismatch\n";
        return 6;
    }
    if (!null_commands->submitted()) {
        std::cerr << "command list was not submitted\n";
        return 7;
    }

    // D2 color policy: the null backend accepts both 8-bit sRGB formats (same 4-byte layout as
    // their UNORM siblings) for textures, targets, and pipelines. It records commands only, so it
    // does not emulate the encode itself.
    const rhi::TextureFormat srgb_formats[] = {rhi::TextureFormat::R8G8B8A8Srgb,
                                               rhi::TextureFormat::B8G8R8A8Srgb};
    for (const rhi::TextureFormat format : srgb_formats) {
        const std::uint8_t pixel[4] = {10, 20, 30, 40};
        rhi::TextureDesc texture_desc;
        texture_desc.extent = {1, 1};
        texture_desc.format = format;
        texture_desc.pixels = pixel;
        const auto srgb_texture = device->create_texture(texture_desc);

        rhi::RenderTargetDesc target_desc;
        target_desc.extent = {4, 4};
        target_desc.format = format;
        const auto srgb_target = device->create_render_target(target_desc);

        rhi::PipelineDesc pipeline_desc;
        pipeline_desc.color_format = format;
        const auto srgb_pipeline = device->create_pipeline(pipeline_desc);

        if (srgb_texture->format() != format || srgb_target->format() != format ||
            srgb_pipeline == nullptr || !rhi::is_srgb(format)) {
            std::cerr << "sRGB format not accepted by the null backend\n";
            return 8;
        }
    }
    if (rhi::is_srgb(rhi::TextureFormat::R8G8B8A8Unorm) ||
        rhi::is_srgb(rhi::TextureFormat::R8Unorm) ||
        rhi::is_srgb(rhi::TextureFormat::B8G8R8A8Unorm)) {
        std::cerr << "is_srgb reports a UNORM format as sRGB\n";
        return 9;
    }

    // D7 defaults: a backend without a loss/recovery path stays Ready and reports the recovery
    // request as NotSupported instead of pretending to recover.
    if (device->status() != rhi::DeviceStatus::Ready) {
        std::cerr << "null device should always be Ready\n";
        return 10;
    }
    const auto recovery = device->recover();
    if (recovery.has_value() || recovery.error() != rhi::DeviceRecoveryError::NotSupported) {
        std::cerr << "null device recovery should report NotSupported\n";
        return 11;
    }

    std::cout << "rhi_null: ok\n";
    return 0;
}
