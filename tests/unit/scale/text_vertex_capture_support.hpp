// SPDX-License-Identifier: MIT
//
// Test support: read the quad a Batched2DRenderer emitted back out of the vertex buffer, through
// the null device. A null-backend draw record carries counts, not positions, so a test that asserts
// quad geometry needs the vertices themselves. This decorator records the buffer the renderer binds
// (the arena writes it immediately before binding, so the data is final at that point) and exposes
// the first quad's four corners afterwards. Everything else forwards to the null device unchanged.

#pragma once

#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace tessera::testsupport {

namespace rhi = tessera::rendering::rhi;

// One textured quad as the renderer emitted it, in target/CSS pixels (the frame transform is
// identity in these tests, so the two coincide).
struct EmittedQuad {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float uv_width = 0.0f;
    float uv_height = 0.0f;
    bool valid = false;
};

class RecordingDevice;

// Forwards every command and records the vertex buffer the renderer binds.
class RecordingCommandList final : public rhi::CommandList {
public:
    RecordingCommandList(std::unique_ptr<rhi::CommandList> inner, RecordingDevice& device)
        : inner_(std::move(inner)),
          device_(device) {}

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override {
        inner_->begin_render_pass(target, desc);
    }
    void end_render_pass() override {
        inner_->end_render_pass();
    }
    void bind_pipeline(const rhi::Pipeline& pipeline) override {
        inner_->bind_pipeline(pipeline);
    }
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override {
        inner_->bind_texture(slot, texture);
    }
    void bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) override {
        inner_->bind_sampled_target(slot, target);
    }
    void set_viewport(const rhi::Viewport& viewport) override {
        inner_->set_viewport(viewport);
    }
    void set_scissor(const rhi::Scissor& scissor) override {
        inner_->set_scissor(scissor);
    }
    void push_constants(const void* data, std::size_t size) override {
        inner_->push_constants(data, size);
    }
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) override {
        inner_->draw(vertex_count, first_vertex);
    }
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override {
        inner_->copy_target_to_buffer(target, buffer);
    }
    void copy_target_region(const rhi::RenderTarget& source, const rhi::Scissor& source_region,
                            const rhi::RenderTarget& destination,
                            rhi::Offset2D destination_offset = {}) override {
        inner_->copy_target_region(source, source_region, destination, destination_offset);
    }
    void blit_target(const rhi::RenderTarget& source, const rhi::RenderTarget& destination,
                     rhi::Filter filter) override {
        inner_->blit_target(source, destination, filter);
    }
    void submit() override {
        inner_->submit();
    }
    void submit_async() override {
        inner_->submit_async();
    }
    [[nodiscard]] bool is_complete() const override {
        return inner_->is_complete();
    }
    [[nodiscard]] bool wait(std::uint64_t timeout_ms) override {
        return inner_->wait(timeout_ms);
    }

    // The null backend's counters, so a test keeps asserting draws/vertices/texture binds through
    // the decorator. Null when the wrapped list is not the null backend.
    [[nodiscard]] const tessera::rendering::null::NullCommandStats* stats() const {
        const auto* null_list =
            dynamic_cast<const tessera::rendering::null::NullCommandList*>(inner_.get());
        return null_list != nullptr ? &null_list->stats() : nullptr;
    }

private:
    std::unique_ptr<rhi::CommandList> inner_;
    RecordingDevice& device_;
};

// What the texture uploads of a RecordingDevice cost: how many whole-image and region writes ran
// and how many bytes each moved (issue #681: a one-glyph atlas change must move its rectangle, not
// the page).
struct TextureUploadStats {
    std::size_t whole_writes = 0;
    std::size_t whole_bytes = 0;
    std::size_t region_writes = 0;
    std::size_t region_bytes = 0;
};

// Forwards to the null texture, counts every write, and keeps the bytes the "GPU" holds so a test
// can compare them with the CPU copy the uploads were supposed to mirror.
class CountingTexture final : public rhi::Texture {
public:
    CountingTexture(std::unique_ptr<rhi::Texture> inner, const rhi::TextureDesc& desc,
                    TextureUploadStats& stats, std::vector<const CountingTexture*>& registry)
        : inner_(std::move(inner)),
          stats_(&stats),
          registry_(&registry),
          pixel_bytes_(desc.format == rhi::TextureFormat::R8Unorm ? 1U : 4U) {
        shadow_.assign(
            static_cast<std::size_t>(desc.extent.width) * desc.extent.height * pixel_bytes_, 0);
        if (desc.pixels != nullptr) {
            const std::size_t pitch =
                desc.row_pitch != 0 ? desc.row_pitch
                                    : static_cast<std::size_t>(desc.extent.width) * pixel_bytes_;
            for (std::uint32_t row = 0; row < desc.extent.height; ++row) {
                std::memcpy(shadow_.data() +
                                static_cast<std::size_t>(row) * desc.extent.width * pixel_bytes_,
                            static_cast<const std::uint8_t*>(desc.pixels) + row * pitch,
                            static_cast<std::size_t>(desc.extent.width) * pixel_bytes_);
            }
        }
        registry_->push_back(this);
    }
    CountingTexture(const CountingTexture&) = delete;
    CountingTexture& operator=(const CountingTexture&) = delete;
    ~CountingTexture() override {
        registry_->erase(std::remove(registry_->begin(), registry_->end(), this), registry_->end());
    }

    [[nodiscard]] rhi::Extent2D extent() const override {
        return inner_->extent();
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return inner_->format();
    }
    [[nodiscard]] std::uint32_t levels() const override {
        return inner_->levels();
    }
    void write(const void* data, std::size_t size) override {
        inner_->write(data, size);
        ++stats_->whole_writes;
        stats_->whole_bytes += size;
        if (data != nullptr && size == shadow_.size()) {
            std::memcpy(shadow_.data(), data, size);
        }
    }
    void write_region(const rhi::TextureRegion& region, const void* data,
                      std::size_t row_pitch) override {
        inner_->write_region(region, data, row_pitch);
        if (region.empty() || data == nullptr) {
            return;
        }
        const std::size_t pitch =
            row_pitch != 0 ? row_pitch : static_cast<std::size_t>(region.width) * pixel_bytes_;
        ++stats_->region_writes;
        stats_->region_bytes +=
            static_cast<std::size_t>(region.width) * region.height * pixel_bytes_;
        const std::uint32_t width = inner_->extent().width;
        for (std::uint32_t row = 0; row < region.height; ++row) {
            std::memcpy(
                shadow_.data() +
                    (static_cast<std::size_t>(region.y + row) * width + region.x) * pixel_bytes_,
                static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(row) * pitch,
                static_cast<std::size_t>(region.width) * pixel_bytes_);
        }
    }

    // The bytes the GPU copy holds after every write so far.
    [[nodiscard]] const std::vector<std::uint8_t>& gpu_bytes() const {
        return shadow_;
    }

private:
    std::unique_ptr<rhi::Texture> inner_;
    TextureUploadStats* stats_;
    std::vector<const CountingTexture*>* registry_;
    std::size_t pixel_bytes_;
    std::vector<std::uint8_t> shadow_;
};

// A null device whose command lists record the bound vertex buffer and whose textures count their
// uploads.
class RecordingDevice final : public rhi::Device {
public:
    RecordingDevice()
        : inner_(tessera::rendering::null::create_null_device({})) {}

    [[nodiscard]] rhi::Backend backend() const override {
        return inner_->backend();
    }
    [[nodiscard]] bool supports_dual_source_blend() const override {
        return inner_->supports_dual_source_blend();
    }
    [[nodiscard]] bool supports_texture_mipmaps(rhi::TextureFormat format) const override {
        return inner_->supports_texture_mipmaps(format);
    }
    [[nodiscard]] bool supports_storage_buffers() const override {
        return inner_->supports_storage_buffers();
    }
    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return inner_->create_buffer(desc);
    }
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        return std::make_unique<CountingTexture>(inner_->create_texture(desc), desc,
                                                 texture_uploads_, live_textures_);
    }
    // The textures created through this device that are still alive, oldest first (a test reads the
    // bytes the "GPU" holds for the texture a renderer made privately).
    [[nodiscard]] const std::vector<const CountingTexture*>& live_textures() const {
        return live_textures_;
    }
    [[nodiscard]] const TextureUploadStats& texture_uploads() const {
        return texture_uploads_;
    }
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override {
        return inner_->create_pipeline(desc);
    }
    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override {
        return inner_->create_render_target(desc);
    }
    std::unique_ptr<rhi::CommandList> create_command_list() override {
        return std::make_unique<RecordingCommandList>(inner_->create_command_list(), *this);
    }
    std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override {
        return inner_->create_swapchain(desc);
    }
    void wait_idle() override {
        inner_->wait_idle();
    }

    // The first quad the renderer wrote: emit_quad emits six vertices, two triangles over the
    // corners (x0,y0) (x1,y0) (x1,y1) (x0,y1), so the corner at index 0 and the one at index 2 are
    // the top-left and bottom-right of the quad.
    [[nodiscard]] EmittedQuad first_quad() const {
        EmittedQuad quad;
        if (vertex_buffer_ == nullptr ||
            vertex_buffer_->size() < 4 * tessera::rendering::gpu::kVertex2DStride) {
            return quad;
        }
        std::array<tessera::rendering::gpu::Vertex2D, 4> corners{};
        vertex_buffer_->read(corners.data(),
                             corners.size() * tessera::rendering::gpu::kVertex2DStride, 0);
        quad.left = corners[0].x;
        quad.top = corners[0].y;
        quad.width = corners[2].x - corners[0].x;
        quad.height = corners[2].y - corners[0].y;
        quad.u = corners[0].u;
        quad.v = corners[0].v;
        quad.uv_width = corners[2].u - corners[0].u;
        quad.uv_height = corners[2].v - corners[0].v;
        quad.valid = true;
        return quad;
    }

private:
    friend class RecordingCommandList;

    std::unique_ptr<rhi::Device> inner_;
    mutable const rhi::Buffer* vertex_buffer_ = nullptr;
    TextureUploadStats texture_uploads_;
    std::vector<const CountingTexture*> live_textures_;
};

inline void RecordingCommandList::bind_vertex_buffer(const rhi::Buffer& buffer) {
    device_.vertex_buffer_ = &buffer;
    inner_->bind_vertex_buffer(buffer);
}

} // namespace tessera::testsupport
