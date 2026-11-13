// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/null/null_swapchain.hpp"

#include <algorithm>
#include <cstring>
#include <expected>
#include <stdexcept>
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

class NullBuffer final : public rhi::Buffer {
public:
    explicit NullBuffer(std::size_t size)
        : data_(size, 0) {}

    [[nodiscard]] std::size_t size() const override {
        return data_.size();
    }

    void write(const void* data, std::size_t size, std::size_t offset) override {
        check_range(size, offset);
        std::memcpy(data_.data() + offset, data, size);
    }

    void read(void* data, std::size_t size, std::size_t offset) const override {
        check_range(size, offset);
        std::memcpy(data, data_.data() + offset, size);
    }

private:
    // Matches the Vulkan backend: a range outside the buffer is a caller bug, not a no-op.
    void check_range(std::size_t size, std::size_t offset) const {
        if (size > data_.size() || offset > data_.size() - size) {
            throw std::out_of_range("NullBuffer: range exceeds the buffer size");
        }
    }

    std::vector<std::uint8_t> data_;
};

class NullTexture final : public rhi::Texture {
public:
    explicit NullTexture(const rhi::TextureDesc& desc)
        : desc_(desc) {
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("NullTexture: unsupported sample count");
        }
        if (desc.sample_count > 1 && desc.pixels != nullptr) {
            throw std::invalid_argument(
                "NullTexture: pixels cannot be uploaded to a multisampled texture");
        }
        const std::size_t bytes = bytes_per_pixel(desc.format);
        data_.assign(static_cast<std::size_t>(desc.extent.width) * desc.extent.height * bytes, 0);
        if (desc.pixels != nullptr && bytes != 0) {
            const std::size_t row_pitch = desc.row_pitch != 0
                                              ? desc.row_pitch
                                              : static_cast<std::size_t>(desc.extent.width) * bytes;
            for (std::uint32_t row = 0; row < desc.extent.height; ++row) {
                std::memcpy(data_.data() +
                                static_cast<std::size_t>(row) * desc.extent.width * bytes,
                            static_cast<const std::uint8_t*>(desc.pixels) + row * row_pitch,
                            static_cast<std::size_t>(desc.extent.width) * bytes);
            }
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
    void write(const void* data, std::size_t size) override {
        if (desc_.sample_count > 1) {
            throw std::invalid_argument("NullTexture: multisampled textures cannot be uploaded");
        }
        if (data == nullptr || size != data_.size()) {
            return;
        }
        std::memcpy(data_.data(), data, size);
    }

private:
    static std::size_t bytes_per_pixel(rhi::TextureFormat format) {
        return format == rhi::TextureFormat::R8Unorm ? 1 : 4;
    }

    rhi::TextureDesc desc_;
    std::vector<std::uint8_t> data_;
};

class NullPipeline final : public rhi::Pipeline {
public:
    explicit NullPipeline(std::uint32_t sample_count)
        : sample_count_(sample_count) {}

    [[nodiscard]] std::uint32_t sample_count() const override {
        return sample_count_;
    }

private:
    std::uint32_t sample_count_ = 1;
};

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

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return std::make_unique<NullBuffer>(desc.size);
    }

    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("null create_texture: unsupported sample count");
        }
        return std::make_unique<NullTexture>(desc);
    }

    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override {
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("null create_pipeline: unsupported sample count");
        }
        return std::make_unique<NullPipeline>(desc.sample_count);
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
