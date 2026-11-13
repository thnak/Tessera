// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::null {

struct NullDrawRecord {
    rhi::Scissor scissor;
    std::uint32_t vertex_count = 0;
    std::uint32_t first_vertex = 0;
};

// One copy_target_region() request, in recording order. The null backend moves no pixels; the
// record is what structural tests assert (the region and where it was placed).
struct NullTargetRegionCopy {
    rhi::Scissor source_region;
    rhi::Offset2D destination_offset;
    rhi::Extent2D source_extent;
    rhi::Extent2D destination_extent;
};

struct NullCommandStats {
    std::uint32_t begin_render_passes = 0;
    std::uint32_t end_render_passes = 0;
    std::uint32_t pipeline_binds = 0;
    std::uint32_t vertex_buffer_binds = 0;
    std::uint32_t texture_binds = 0;
    std::uint32_t scissor_changes = 0;
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    // blit_target() calls recorded (the null backend resamples nothing; it counts the request).
    std::uint32_t blits = 0;
    // copy_target_region() calls recorded, with one entry each in target_region_copies.
    std::uint32_t region_copies = 0;
    std::vector<NullTargetRegionCopy> target_region_copies;
    // Sample count of the most recent begin_render_pass target (1 or 4).
    std::uint32_t last_render_pass_sample_count = 0;
    // One entry per draw, in recording order; `scissor` is the state active at draw time.
    std::vector<NullDrawRecord> draw_records;
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
    void copy_target_region(const rhi::RenderTarget& source, const rhi::Scissor& source_region,
                            const rhi::RenderTarget& destination,
                            rhi::Offset2D destination_offset = {}) override;
    void blit_target(const rhi::RenderTarget& source, const rhi::RenderTarget& destination,
                     rhi::Filter filter) override;
    void submit() override;
    void submit_async() override;
    [[nodiscard]] bool is_complete() const override;
    [[nodiscard]] bool wait(std::uint64_t timeout_ms) override;

    [[nodiscard]] const NullCommandStats& stats() const {
        return stats_;
    }
    [[nodiscard]] bool submitted() const {
        return submitted_;
    }

private:
    NullCommandStats stats_;
    rhi::Scissor scissor_;
    bool submitted_ = false;
    bool in_pass_ = false;
    std::uint32_t pass_sample_count_ = 1;
};

std::unique_ptr<rhi::Device> create_null_device(const rhi::DeviceDesc& desc);

} // namespace tessera::rendering::null
