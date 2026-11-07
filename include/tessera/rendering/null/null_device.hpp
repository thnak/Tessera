// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::null {

struct NullCommandStats {
    std::uint32_t begin_render_passes = 0;
    std::uint32_t end_render_passes = 0;
    std::uint32_t pipeline_binds = 0;
    std::uint32_t vertex_buffer_binds = 0;
    std::uint32_t texture_binds = 0;
    std::uint32_t scissor_changes = 0;
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
};

class NullCommandList final : public rhi::CommandList {
public:
    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override;
    void end_render_pass() override;
    void bind_pipeline(const rhi::Pipeline& pipeline) override;
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override;
    void set_viewport(const rhi::Viewport& viewport) override;
    void set_scissor(const rhi::Scissor& scissor) override;
    void push_constants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) override;
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override;
    void submit() override;

    [[nodiscard]] const NullCommandStats& stats() const {
        return stats_;
    }
    [[nodiscard]] bool submitted() const {
        return submitted_;
    }

private:
    NullCommandStats stats_;
    bool submitted_ = false;
};

std::unique_ptr<rhi::Device> create_null_device(const rhi::DeviceDesc& desc);

} // namespace tessera::rendering::null
