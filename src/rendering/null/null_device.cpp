// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"

#include <algorithm>
#include <cstring>
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
    explicit NullTexture(rhi::TextureDesc desc)
        : desc_(std::move(desc)) {}

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }

private:
    rhi::TextureDesc desc_;
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

    void wait_idle() override {}
};

} // namespace

void NullCommandList::begin_render_pass(const rhi::RenderTarget&, const rhi::RenderTargetDesc&) {
    ++stats_.begin_render_passes;
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

void NullCommandList::set_scissor(const rhi::Scissor&) {
    ++stats_.scissor_changes;
}

void NullCommandList::push_constants(const void*, std::size_t) {}

void NullCommandList::draw(std::uint32_t vertex_count, std::uint32_t) {
    ++stats_.draws;
    stats_.vertices += vertex_count;
}

void NullCommandList::copy_target_to_buffer(const rhi::RenderTarget&, rhi::Buffer&) {}

void NullCommandList::submit() {
    submitted_ = true;
}

std::unique_ptr<rhi::Device> create_null_device(const rhi::DeviceDesc&) {
    return std::make_unique<NullDevice>();
}

} // namespace tessera::rendering::null
