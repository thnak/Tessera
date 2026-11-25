// SPDX-License-Identifier: MIT

#include "null_resources.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/null/null_swapchain.hpp"

#include <algorithm>
#include <cstring>
#include <expected>
#include <stdexcept>
#include <string>
#include <utility>

namespace tessera::rendering::null {
namespace {

static_assert(rhi::kRhiVersionMajor == 1, "Null backend implements RHI 1.x");

// Half-open integer region inside `extent`. A zero or negative size is invalid, not a no-op: the
// RHI reports caller bugs instead of clipping silently.
bool region_fits(const rhi::Scissor& region, rhi::Extent2D extent) {
    if (region.x < 0 || region.y < 0 || region.width == 0 || region.height == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(region.x) + region.width <= extent.width &&
           static_cast<std::uint64_t>(region.y) + region.height <= extent.height;
}

bool offset_region_fits(rhi::Offset2D offset, const rhi::Scissor& region, rhi::Extent2D extent) {
    if (offset.x < 0 || offset.y < 0) {
        return false;
    }
    return static_cast<std::uint64_t>(offset.x) + region.width <= extent.width &&
           static_cast<std::uint64_t>(offset.y) + region.height <= extent.height;
}

class NullRenderTarget final : public rhi::RenderTarget {
public:
    explicit NullRenderTarget(rhi::RenderTargetDesc desc)
        : desc_(std::move(desc)) {
        if (!rhi::is_supported_sample_count(desc_.sample_count)) {
            throw std::invalid_argument("NullRenderTarget: unsupported sample count");
        }
    }

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return desc_.sample_count;
    }
    [[nodiscard]] rhi::Filter filter() const override {
        return desc_.filter;
    }
    [[nodiscard]] rhi::AddressMode address() const override {
        return desc_.address;
    }

    // Content tracking, as in the Vulkan backend: a target holds known pixels only once a render
    // pass closed on it or a copy/blit wrote it, and only then may it be read or sampled.
    void mark_content_initialized() const {
        content_initialized_ = true;
    }
    [[nodiscard]] bool content_initialized() const {
        return content_initialized_;
    }

private:
    rhi::RenderTargetDesc desc_;
    mutable bool content_initialized_ = false;
};

// The null backend's own target type. A command list only ever records null targets, so a failed
// cast means a caller mixed backends (a programming error the RHI docs call out).
const NullRenderTarget* as_null_target(const rhi::RenderTarget& target) {
    return dynamic_cast<const NullRenderTarget*>(&target);
}

void mark_target_content(const rhi::RenderTarget& target) {
    if (const NullRenderTarget* concrete = as_null_target(target)) {
        concrete->mark_content_initialized();
    }
}

class NullDevice final : public rhi::Device {
public:
    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::Null;
    }

    [[nodiscard]] rhi::RhiCapabilities capabilities() const override {
        return rhi::RhiCapabilities{rhi::Backend::Null, rhi::kRhiVersion, timestamp_capability()};
    }

    // Null parity: declares 4x support for every format it accepts so MSAA code paths are testable
    // without a GPU. It enforces the same 1/4 validation rules as the Vulkan backend.
    [[nodiscard]] bool supports_sample_count(rhi::TextureFormat,
                                             std::uint32_t sample_count) const override {
        return rhi::is_supported_sample_count(sample_count);
    }

    // Null parity for the dual-source capability (RHI 1.5, issue #471). The null backend consumes
    // no fragment output and rasterizes nothing, so every blend mode the RHI defines is equally
    // representable: it records the request (NullPipeline::blend_mode) instead of blending.
    // Reporting true is what lets the structural tests exercise the mode on any machine, and it is
    // paired with that record so "supported" is never an unobserved claim; a test asserts the
    // recorded mode.
    [[nodiscard]] bool supports_dual_source_blend() const override {
        return true;
    }
    // The null device records a mipmapped texture's level count (`Texture::levels()`) so a test can
    // assert what the caller asked for; there is no pixel chain to build.
    [[nodiscard]] bool supports_texture_mipmaps(rhi::TextureFormat) const override {
        return true;
    }

    // Null parity for the storage-buffer capability (RHI 1.12, ADR-068). The null backend reads no
    // shader data, so a read-only storage buffer is representable: it records the bind
    // (NullStorageBufferBind) instead of reading it. Reporting true is what lets the structural
    // tests exercise the path on any machine, and it is paired with that record so "supported" is
    // never an unobserved claim; a test asserts the recorded slot and size.
    [[nodiscard]] bool supports_storage_buffers() const override {
        return true;
    }

    // Null parity for the buffer-to-texture copy (RHI 1.17, ADR-072). The null buffers and textures
    // keep real bytes, so the copy moves them (and validates exactly what Vulkan validates); it is
    // paired with a record (NullBufferTextureCopy) so "supported" is never an unobserved claim.
    // Compute stays unsupported, so a staging buffer is the source a null test can fill.
    [[nodiscard]] bool supports_buffer_texture_copy() const override {
        return true;
    }

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        // Validating-backend parity: Vulkan cannot create a zero-size buffer, so the null backend
        // refuses the same request with the same error (RHI 1.12).
        if (desc.size == 0) {
            throw std::invalid_argument("null create_buffer: a buffer needs a non-zero size");
        }
        return std::make_unique<NullBuffer>(desc.size, desc.usage);
    }

    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        if (!rhi::valid_texture_levels(desc)) {
            throw std::invalid_argument("null create_texture: invalid mip level count");
        }
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("null create_texture: unsupported sample count");
        }
        return std::make_unique<NullTexture>(desc);
    }

    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override {
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("null create_pipeline: unsupported sample count");
        }
        // The whole descriptor is kept, not just the sample count: the blend mode is the state RHI
        // 1.5's dual-source contract is about, and a structural test has no pixels to inspect
        // (issue #471). Nothing is rejected here because nothing is blended.
        return std::make_unique<NullPipeline>(desc);
    }

    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override {
        return std::make_unique<NullRenderTarget>(desc);
    }

    std::unique_ptr<rhi::CommandList> create_command_list() override {
        return std::make_unique<NullCommandList>();
    }

    std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override {
        if (desc.image_count < 2) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        std::unique_ptr<rhi::Swapchain> swapchain = std::make_unique<NullSwapchain>(desc);
        return swapchain;
    }

    void wait_idle() override {}
};

} // namespace

void NullCommandList::begin_render_pass(const rhi::RenderTarget& target,
                                        const rhi::RenderTargetDesc& desc) {
    if (in_pass_) {
        throw std::logic_error("null begin_render_pass: the previous render pass is still active");
    }
    if (desc.sample_count != target.sample_count()) {
        throw std::invalid_argument(
            "null begin_render_pass: descriptor sample count does not match the target");
    }
    ++stats_.begin_render_passes;
    stats_.command_order.push_back(NullCommandKind::BeginRenderPass);
    stats_.last_render_pass_sample_count = target.sample_count();
    pass_sample_count_ = target.sample_count();
    current_target_ = &target;
    in_pass_ = true;
    scissor_ = rhi::Scissor{0, 0, desc.extent.width, desc.extent.height};
}

void NullCommandList::end_render_pass() {
    ++stats_.end_render_passes;
    stats_.command_order.push_back(NullCommandKind::EndRenderPass);
    if (current_target_ != nullptr) {
        mark_target_content(*current_target_);
        current_target_ = nullptr;
    }
    in_pass_ = false;
}

void NullCommandList::bind_pipeline(const rhi::Pipeline& pipeline) {
    if (in_pass_ && pipeline.sample_count() != pass_sample_count_) {
        throw std::invalid_argument(
            "null bind_pipeline: pipeline sample count does not match the render target");
    }
    // The null backend's own pipeline type carries the layout facts a later bind needs (RHI 1.12).
    // A pipeline from another backend is a contract violation the RHI docs call out; the cast is
    // unchecked in release builds like every other backend's.
    current_pipeline_ = dynamic_cast<const NullPipeline*>(&pipeline);
    ++stats_.pipeline_binds;
    stats_.command_order.push_back(NullCommandKind::BindPipeline);
}

void NullCommandList::bind_vertex_buffer(const rhi::Buffer&) {
    ++stats_.vertex_buffer_binds;
}

void NullCommandList::bind_texture(std::uint32_t, const rhi::Texture&) {
    ++stats_.texture_binds;
    stats_.command_order.push_back(NullCommandKind::BindTexture);
}

void NullCommandList::bind_texture_array(std::uint32_t,
                                         const std::vector<const rhi::Texture*>& textures) {
    // An empty array is a no-op on every backend, so a caller that resolves no textures this batch
    // does not have to special-case the call (RHI 1.7, issue #480 part 2).
    if (textures.empty()) {
        return;
    }
    // Validating-backend parity: the null device reports no texture array, so this is the same
    // misuse the Vulkan backend rejects. A structural test on the null backend stays meaningful
    // only if the fallback contract is enforced here too.
    throw std::runtime_error("bind_texture_array: this device has no texture array");
}

void NullCommandList::bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) {
    // Validating-backend parity: the same misuse the Vulkan and WebGPU backends reject, so a
    // structural test on the null backend stays meaningful.
    if (in_pass_) {
        throw std::logic_error("null bind_sampled_target: render pass is still active");
    }
    if (target.presentable()) {
        throw std::invalid_argument("null bind_sampled_target: a swapchain image is not samplable");
    }
    const NullRenderTarget* concrete = as_null_target(target);
    if (concrete == nullptr || !concrete->content_initialized()) {
        throw std::invalid_argument("null bind_sampled_target: the target has no rendered content");
    }
    ++stats_.sampled_target_binds;
    stats_.command_order.push_back(NullCommandKind::BindSampledTarget);
    stats_.sampled_targets.push_back(NullSampledTargetBind{slot, target.extent(), target.format(),
                                                           target.sample_count(), target.filter(),
                                                           target.address()});
}

void NullCommandList::set_viewport(const rhi::Viewport&) {}
void NullCommandList::set_scissor(const rhi::Scissor& scissor) {
    scissor_ = scissor;
    ++stats_.scissor_changes;
}

void NullCommandList::push_constants(const void*, std::size_t) {}

void NullCommandList::draw(std::uint32_t vertex_count, std::uint32_t first_vertex) {
    ++stats_.draws;
    stats_.vertices += vertex_count;
    stats_.command_order.push_back(NullCommandKind::Draw);
    stats_.draw_records.push_back(NullDrawRecord{scissor_, vertex_count, first_vertex});
}

void NullCommandList::copy_target_to_buffer(const rhi::RenderTarget&, rhi::Buffer&) {}

void NullCommandList::copy_target_region(const rhi::RenderTarget& source,
                                         const rhi::Scissor& source_region,
                                         const rhi::RenderTarget& destination,
                                         rhi::Offset2D destination_offset) {
    // Validating-backend parity: the null backend rejects the same misuse the Vulkan and WebGPU
    // backends do, so structural tests on it stay meaningful.
    if (in_pass_) {
        throw std::logic_error("null copy_target_region: render pass is still active");
    }
    if (&source == &destination) {
        throw std::invalid_argument(
            "null copy_target_region: source and destination must be different targets");
    }
    if (source.sample_count() != 1 || destination.sample_count() != 1) {
        throw std::invalid_argument(
            "null copy_target_region: multisampled targets are not supported");
    }
    if (source.presentable()) {
        throw std::invalid_argument(
            "null copy_target_region: a swapchain image is not a copy source");
    }
    if (destination.presentable()) {
        throw std::invalid_argument(
            "null copy_target_region: a swapchain image is not a copy destination");
    }
    if (source.format() != destination.format()) {
        throw std::invalid_argument(
            "null copy_target_region: source and destination formats differ");
    }
    if (!region_fits(source_region, source.extent())) {
        throw std::invalid_argument("null copy_target_region: source region exceeds the target");
    }
    if (!offset_region_fits(destination_offset, source_region, destination.extent())) {
        throw std::invalid_argument(
            "null copy_target_region: destination offset plus region exceeds the target");
    }
    ++stats_.region_copies;
    stats_.command_order.push_back(NullCommandKind::CopyTargetRegion);
    stats_.target_region_copies.push_back(NullTargetRegionCopy{
        source_region, destination_offset, source.extent(), destination.extent()});
    mark_target_content(destination);
}

void NullCommandList::blit_target(const rhi::RenderTarget& source,
                                  const rhi::RenderTarget& destination, rhi::Filter) {
    if (source.sample_count() != 1 || destination.sample_count() != 1) {
        throw std::invalid_argument("null blit_target: multisampled targets are not supported");
    }
    ++stats_.blits;
    stats_.command_order.push_back(NullCommandKind::Blit);
    mark_target_content(destination);
}

void NullCommandList::copy_buffer_to_texture(const rhi::Buffer& source, rhi::Texture& destination,
                                             const rhi::BufferTextureCopy& copy) {
    // Validating-backend parity: the same refusals the Vulkan backend makes, in the same order.
    if (in_pass_) {
        throw std::logic_error("null copy_buffer_to_texture: render pass is still active");
    }
    const auto* buffer = dynamic_cast<const NullBuffer*>(&source);
    auto* texture = dynamic_cast<NullTexture*>(&destination);
    if (buffer == nullptr || texture == nullptr) {
        throw std::invalid_argument(
            "null copy_buffer_to_texture: the buffer or the texture belongs to another backend");
    }
    if (buffer->usage() != rhi::BufferUsage::Staging &&
        buffer->usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "null copy_buffer_to_texture: the source must be a Staging or Storage buffer");
    }
    if (texture->sample_count() != 1) {
        throw std::invalid_argument(
            "null copy_buffer_to_texture: multisampled textures cannot be copied into");
    }
    if (texture->levels() != 1) {
        throw std::invalid_argument(
            "null copy_buffer_to_texture: mipmapped textures are immutable");
    }
    const std::size_t pitch = rhi::checked_buffer_texture_copy(texture->extent(), texture->format(),
                                                               copy, buffer->size());
    if (copy.region.empty()) {
        return;
    }
    // Repack the rows into a tight rectangle, then write it as a region write.
    const std::size_t row_bytes =
        static_cast<std::size_t>(copy.region.width) * rhi::texture_format_bytes(texture->format());
    std::vector<std::uint8_t> rows(row_bytes * copy.region.height);
    for (std::uint32_t row = 0; row < copy.region.height; ++row) {
        buffer->read(rows.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes,
                     copy.buffer_offset + static_cast<std::size_t>(row) * pitch);
    }
    texture->write_region(copy.region, rows.data(), 0);
    stats_.command_order.push_back(NullCommandKind::CopyBufferToTexture);
    stats_.buffer_texture_copies.push_back(
        NullBufferTextureCopy{copy.region, copy.buffer_offset, pitch, buffer->size(),
                              buffer->usage(), texture->extent(), texture->format()});
}

void NullCommandList::submit() {
    stats_.command_order.push_back(NullCommandKind::Submit);
    submitted_ = true;
}

// The null backend has no GPU, so asynchronous submission completes immediately.
void NullCommandList::submit_async() {
    stats_.command_order.push_back(NullCommandKind::Submit);
    submitted_ = true;
}

bool NullCommandList::is_complete() const {
    return submitted_;
}

bool NullCommandList::wait(std::uint64_t) {
    return submitted_;
}

std::unique_ptr<rhi::Device> create_null_device(const rhi::DeviceDesc&) {
    return std::make_unique<NullDevice>();
}

} // namespace tessera::rendering::null
