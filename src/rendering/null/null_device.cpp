// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/null/null_swapchain.hpp"

#include <algorithm>
#include <cstring>
#include <expected>
#include <utility>

namespace tessera::rendering::null {
namespace {

class NullBuffer final : public rhi::Buffer {
public:
    explicit NullBuffer(std::size_t size)
        : data_(size, 0) {}

    [[nodiscard]] std::size_t size() const override {
        return data_.size();
    }

    void write(const void* data, std::size_t size, std::size_t offset) override {
        if (offset + size > data_.size()) {
            return;
        }
        std::memcpy(data_.data() + offset, data, size);
    }

    void read(void* data, std::size_t size, std::size_t offset) const override {
        if (offset + size > data_.size()) {
            return;
        }
        std::memcpy(data, data_.data() + offset, size);
    }

private:
    std::vector<std::uint8_t> data_;
};

class NullTexture final : public rhi::Texture {
public:
    explicit NullTexture(const rhi::TextureDesc& desc)
        : desc_(desc) {
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
    void write(const void* data, std::size_t size) override {
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

class NullPipeline final : public rhi::Pipeline {};

class NullRenderTarget final : public rhi::RenderTarget {
public:
    explicit NullRenderTarget(rhi::RenderTargetDesc desc)
        : desc_(std::move(desc)) {}

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }

private:
    rhi::RenderTargetDesc desc_;
};

class NullDevice final : public rhi::Device {
public:
    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::Null;
    }

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return std::make_unique<NullBuffer>(desc.size);
    }

    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        return std::make_unique<NullTexture>(desc);
    }

    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc&) override {
        return std::make_unique<NullPipeline>();
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

void NullCommandList::begin_render_pass(const rhi::RenderTarget&,
                                        const rhi::RenderTargetDesc& desc) {
    ++stats_.begin_render_passes;
    scissor_ = rhi::Scissor{0, 0, desc.extent.width, desc.extent.height};
}

void NullCommandList::end_render_pass() {
    ++stats_.end_render_passes;
}

void NullCommandList::bind_pipeline(const rhi::Pipeline&) {
    ++stats_.pipeline_binds;
}

void NullCommandList::bind_vertex_buffer(const rhi::Buffer&) {
    ++stats_.vertex_buffer_binds;
}

void NullCommandList::bind_texture(std::uint32_t, const rhi::Texture&) {
    ++stats_.texture_binds;
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
    stats_.draw_records.push_back(NullDrawRecord{scissor_, vertex_count, first_vertex});
}

void NullCommandList::copy_target_to_buffer(const rhi::RenderTarget&, rhi::Buffer&) {}

void NullCommandList::submit() {
    submitted_ = true;
}

// The null backend has no GPU, so asynchronous submission completes immediately.
void NullCommandList::submit_async() {
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
