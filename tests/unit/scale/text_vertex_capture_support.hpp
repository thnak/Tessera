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

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

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

// A null device whose command lists record the bound vertex buffer.
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
    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return inner_->create_buffer(desc);
    }
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        return inner_->create_texture(desc);
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
};

inline void RecordingCommandList::bind_vertex_buffer(const rhi::Buffer& buffer) {
    device_.vertex_buffer_ = &buffer;
    inner_->bind_vertex_buffer(buffer);
}

} // namespace tessera::testsupport
