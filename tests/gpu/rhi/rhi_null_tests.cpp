// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace tessera::rendering;

    // Pin the published contract version: 1.1 added `copy_target_region` (issue #242); 1.2 added
    // the swapchain composite-alpha request and the resolved-mode query (glass-v1.md section 6,
    // issue #243), both additive compatible changes.
    if (rhi::kRhiVersionMajor != 1 || rhi::kRhiVersionMinor != 2 ||
        rhi::kRhiVersion != ((1u << 16) | 2u)) {
        std::cerr << "unexpected RHI version constants\n";
        return 1;
    }

    auto device = null::create_null_device(rhi::DeviceDesc{});
    if (device->backend() != rhi::Backend::Null) {
        std::cerr << "unexpected backend\n";
        return 1;
    }
    const auto caps = device->capabilities();
    if (caps.backend != rhi::Backend::Null || caps.rhi_version != rhi::kRhiVersion ||
        caps.timestamps.usable()) {
        std::cerr << "unexpected null device capabilities\n";
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

    // copy_target_region: a render-target region copied into another target at an offset. The null
    // backend records the request and rejects the invalid forms a validating backend rejects
    // (RHI 1.1, issue #242).
    auto copy_target = device->create_render_target(rhi::RenderTargetDesc{{32, 32}});
    auto copy_commands = device->create_command_list();
    copy_commands->begin_render_pass(*target, rhi::RenderTargetDesc{{64, 64}});
    copy_commands->end_render_pass();
    copy_commands->copy_target_region(*target, rhi::Scissor{4, 8, 16, 16}, *copy_target,
                                      rhi::Offset2D{2, 3});
    copy_commands->submit();
    auto* copy_null = dynamic_cast<null::NullCommandList*>(copy_commands.get());
    if (copy_null == nullptr || copy_null->stats().region_copies != 1 ||
        copy_null->stats().target_region_copies.size() != 1) {
        std::cerr << "copy_target_region was not recorded\n";
        return 22;
    }
    const null::NullTargetRegionCopy& copy_record = copy_null->stats().target_region_copies.front();
    if (copy_record.source_region.x != 4 || copy_record.destination_offset.y != 3 ||
        copy_record.destination_extent.width != 32) {
        std::cerr << "copy_target_region record mismatch\n";
        return 23;
    }
    bool copy_rejected = false;
    try {
        copy_commands->copy_target_region(*target, rhi::Scissor{60, 60, 8, 8}, *copy_target, {});
    } catch (const std::invalid_argument&) {
        copy_rejected = true;
    }
    if (!copy_rejected) {
        std::cerr << "copy_target_region should reject an out-of-range source region\n";
        return 24;
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

    // MSAA parity: 4x is declared for supported formats, invalid sample counts are rejected, and a
    // pipeline/target sample-count mismatch is rejected at bind time (no silent degradation).
    if (!device->supports_sample_count(rhi::TextureFormat::R8G8B8A8Unorm, 4)) {
        std::cerr << "null device should declare 4x support\n";
        return 12;
    }
    if (device->supports_sample_count(rhi::TextureFormat::R8G8B8A8Unorm, 2)) {
        std::cerr << "null device should reject 2x sample count\n";
        return 13;
    }

    rhi::TextureDesc msaa_texture_desc;
    msaa_texture_desc.extent = {4, 4};
    msaa_texture_desc.sample_count = 4;
    const auto msaa_texture = device->create_texture(msaa_texture_desc);
    if (msaa_texture->sample_count() != 4) {
        std::cerr << "4x texture does not report sample_count 4\n";
        return 14;
    }
    bool rejected = false;
    try {
        const std::uint8_t pixel[4] = {1, 2, 3, 4};
        msaa_texture->write(pixel, sizeof(pixel));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "4x texture should reject uploads\n";
        return 15;
    }
    rejected = false;
    try {
        rhi::TextureDesc invalid_desc;
        invalid_desc.extent = {4, 4};
        invalid_desc.sample_count = 2;
        (void)device->create_texture(invalid_desc);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "invalid texture sample count should be rejected\n";
        return 16;
    }

    rhi::RenderTargetDesc msaa_target_desc;
    msaa_target_desc.extent = {4, 4};
    msaa_target_desc.sample_count = 4;
    const auto msaa_target = device->create_render_target(msaa_target_desc);
    if (msaa_target->sample_count() != 4) {
        std::cerr << "4x render target does not report sample_count 4\n";
        return 17;
    }
    rhi::PipelineDesc msaa_pipeline_desc;
    msaa_pipeline_desc.sample_count = 4;
    const auto msaa_pipeline = device->create_pipeline(msaa_pipeline_desc);
    const auto one_sample_pipeline = device->create_pipeline(rhi::PipelineDesc{});
    if (msaa_pipeline->sample_count() != 4 || one_sample_pipeline->sample_count() != 1) {
        std::cerr << "pipeline sample_count not recorded\n";
        return 18;
    }

    auto msaa_commands = device->create_command_list();
    msaa_commands->begin_render_pass(*msaa_target, msaa_target_desc);
    rejected = false;
    try {
        msaa_commands->bind_pipeline(*one_sample_pipeline);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "1x pipeline should be rejected on a 4x target\n";
        return 19;
    }
    msaa_commands->bind_pipeline(*msaa_pipeline);
    msaa_commands->end_render_pass();
    if (static_cast<null::NullCommandList*>(msaa_commands.get())
            ->stats()
            .last_render_pass_sample_count != 4) {
        std::cerr << "null backend did not record the render pass sample count\n";
        return 20;
    }

    rejected = false;
    try {
        auto mismatched_commands = device->create_command_list();
        rhi::RenderTargetDesc wrong_desc;
        wrong_desc.extent = {4, 4};
        wrong_desc.sample_count = 1;
        mismatched_commands->begin_render_pass(*msaa_target, wrong_desc);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "begin_render_pass should reject a descriptor sample-count mismatch\n";
        return 21;
    }

    std::cout << "rhi_null: ok\n";
    return 0;
}
