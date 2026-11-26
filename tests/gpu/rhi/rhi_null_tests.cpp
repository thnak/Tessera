// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace tessera::rendering;

    // Pin the published contract version: 1.1 added `copy_target_region` (issue #242); 1.2 added
    // the swapchain composite-alpha request and the resolved-mode query (glass-v1.md section 6,
    // issue #243); 1.3 added the sampled view of a render target (`bind_sampled_target` and the
    // target's sampler state, issue #254); 1.4 added `DeviceDesc::surface`/`SurfacePlatform`, the
    // presentation surface family a device is created for (issue #155); 1.5 added
    // `BlendMode::DualSource` and `Device::supports_dual_source_blend()` (issue #471); 1.6 added
    // `TextureDesc::levels` and `Device::supports_texture_mipmaps()` (issue #491); 1.7 added
    // `CommandList::bind_texture_array` and `Device::texture_array_capacity()` (issue #480);
    // 1.8 added `Backend::Metal`, the native macOS/iOS backend (ADR-053); 1.9 added
    // `Texture::write_region`/`TextureRegion` (issue #681); 1.10 added `SurfacePlatform::Android`,
    // the `VK_KHR_android_surface` presentation family (issue #189); 1.11 gave every RHI `enum
    // class` an explicit `std::uint8_t` base type (issue #734), which is an ABI change rather than
    // a source one; 1.12 added `BufferUsage::Storage`, `CommandList::bind_storage_buffer`,
    // `PipelineDesc::storage_buffer` and `Device::supports_storage_buffers()` (ADR-068 slice 1,
    // issue #669); 1.13 stated the color-space contract and moved presentation to UNORM (issue
    // #791), changing the surface format a backend chooses but not a single enumerator or member;
    // 1.14 added `MemoryCategory`, `MemoryCategoryStats`, `TextureDesc::category` and
    // `DeviceMemoryStats::categories` (issue #703), which grew `DeviceMemoryStats` and is therefore
    // an ABI change for anything that copies it; 1.15 added `Pipeline::declares_storage_buffer()`
    // (ADR-068 slice 3, issue #671); 1.16 added compute pipelines, `BufferDesc::gpu_read_write` and
    // the `CommandList` compute commands (ADR-071 spike, issue #683); 1.17 added
    // `CommandList::copy_buffer_to_texture` and its capability (ADR-072); 1.18 added
    // `ComputeCapabilities`, `Device::compute_capabilities()` and `CommandList::dispatch_indirect`
    // (issue #684). All are additive compatible changes.
    if (rhi::kRhiVersionMajor != 1 || rhi::kRhiVersionMinor != 18 ||
        rhi::kRhiVersion != ((1u << 16) | 18u)) {
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

    // RHI 1.6 mip levels (#491): the null device records the count and enforces the contract.
    if (!device->supports_texture_mipmaps(rhi::TextureFormat::R8G8B8A8Unorm)) {
        std::cerr << "the null device should report mip support\n";
        return 22;
    }
    const std::array<std::uint8_t, 8 * 8 * 4> mip_pixels{};
    rhi::TextureDesc mip_desc;
    mip_desc.extent = {8, 8};
    mip_desc.pixels = mip_pixels.data();
    mip_desc.levels = rhi::full_mip_levels(mip_desc.extent);
    auto mip_texture = device->create_texture(mip_desc);
    if (mip_desc.levels != 4 || mip_texture->levels() != 4) {
        std::cerr << "null backend did not record the requested mip level count\n";
        return 23;
    }
    if (device->create_texture(rhi::TextureDesc{{8, 8}})->levels() != 1) {
        std::cerr << "a default texture should have one level\n";
        return 24;
    }
    const auto rejects = [&](const rhi::TextureDesc& bad) {
        try {
            (void)device->create_texture(bad);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    rhi::TextureDesc too_many = mip_desc;
    too_many.levels = 5;
    rhi::TextureDesc no_pixels = mip_desc;
    no_pixels.pixels = nullptr;
    rhi::TextureDesc multisampled = mip_desc;
    multisampled.sample_count = 4;
    rhi::TextureDesc zero_levels = mip_desc;
    zero_levels.levels = 0;
    if (!rejects(too_many) || !rejects(no_pixels) || !rejects(multisampled) ||
        !rejects(zero_levels)) {
        std::cerr << "malformed mip requests should be rejected\n";
        return 25;
    }
    bool write_rejected = false;
    try {
        mip_texture->write(mip_pixels.data(), mip_pixels.size());
    } catch (const std::invalid_argument&) {
        write_rejected = true;
    }
    if (!write_rejected) {
        std::cerr << "a mipmapped texture should be immutable\n";
        return 26;
    }

    // RHI 1.12 storage buffers (ADR-068 slice 1, issue #669): the null device reports the
    // capability, records the bind, and rejects the same misuse a validating backend rejects.
    if (!device->supports_storage_buffers()) {
        std::cerr << "the null device should report storage-buffer support\n";
        return 27;
    }
    const auto storage_buffer =
        device->create_buffer(rhi::BufferDesc{64, rhi::BufferUsage::Storage});
    rhi::PipelineDesc storage_pipeline_desc;
    storage_pipeline_desc.storage_buffer = true;
    const auto storage_pipeline = device->create_pipeline(storage_pipeline_desc);
    const auto* storage_recorded = dynamic_cast<const null::NullPipeline*>(storage_pipeline.get());
    if (storage_recorded == nullptr || !storage_recorded->declares_storage_buffer()) {
        std::cerr << "the null pipeline did not record the storage-buffer layout\n";
        return 28;
    }

    auto storage_commands = device->create_command_list();
    // A bind before any pipeline is a logic error, exactly as on the Vulkan backend.
    bool storage_rejected = false;
    try {
        storage_commands->bind_storage_buffer(0, *storage_buffer);
    } catch (const std::logic_error&) {
        storage_rejected = true;
    }
    if (!storage_rejected) {
        std::cerr << "bind_storage_buffer should reject a bind without a pipeline\n";
        return 29;
    }
    // A pipeline that declares no storage buffer cannot have one bound to it.
    storage_commands->bind_pipeline(*pipeline);
    storage_rejected = false;
    try {
        storage_commands->bind_storage_buffer(0, *storage_buffer);
    } catch (const std::logic_error&) {
        storage_rejected = true;
    }
    if (!storage_rejected) {
        std::cerr << "bind_storage_buffer should reject a pipeline without the layout\n";
        return 30;
    }
    // An empty buffer cannot be read, and Vulkan cannot even create one, so creation refuses it.
    storage_rejected = false;
    try {
        (void)device->create_buffer(rhi::BufferDesc{0, rhi::BufferUsage::Storage});
    } catch (const std::invalid_argument&) {
        storage_rejected = true;
    }
    if (!storage_rejected) {
        std::cerr << "create_buffer should reject a zero-size buffer\n";
        return 31;
    }
    // The valid bind is recorded with its slot and size, which is the structural evidence. The
    // storage buffer is set 1 (ADR-068: a separate set from the sampled image).
    storage_commands->bind_pipeline(*storage_pipeline);
    storage_commands->bind_storage_buffer(1, *storage_buffer);
    const auto* storage_null = dynamic_cast<null::NullCommandList*>(storage_commands.get());
    if (storage_null == nullptr || storage_null->stats().storage_buffer_binds != 1 ||
        storage_null->stats().storage_buffer_records.size() != 1) {
        std::cerr << "bind_storage_buffer was not recorded\n";
        return 32;
    }
    const null::NullStorageBufferBind& storage_record =
        storage_null->stats().storage_buffer_records.front();
    if (storage_record.slot != 1 || storage_record.size != 64) {
        std::cerr << "bind_storage_buffer record mismatch\n";
        return 33;
    }
    // Any other slot is refused: the pipeline layout declares the storage set at 1 only.
    storage_rejected = false;
    try {
        storage_commands->bind_storage_buffer(0, *storage_buffer);
    } catch (const std::invalid_argument&) {
        storage_rejected = true;
    }
    if (!storage_rejected) {
        std::cerr << "bind_storage_buffer should reject a slot other than 1\n";
        return 34;
    }

    std::cout << "rhi_null: ok\n";
    return 0;
}
