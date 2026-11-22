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

// One bind_sampled_target() request (RHI 1.3), with the target's identity at bind time. The null
// backend samples nothing; the record is what a structural test asserts.
struct NullSampledTargetBind {
    std::uint32_t slot = 0;
    rhi::Extent2D extent;
    rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    std::uint32_t sample_count = 1;
    rhi::Filter filter = rhi::Filter::Linear;
    rhi::AddressMode address = rhi::AddressMode::ClampToEdge;
};

// One bind_storage_buffer() request (RHI 1.12, ADR-068), with the buffer's identity at bind time.
// The null backend reads nothing; the record is what a structural test asserts.
struct NullStorageBufferBind {
    std::uint32_t slot = 0;
    std::size_t size = 0;
};

// Every recorded operation, in command-buffer order. The counters below answer "how many"; this log
// answers "in what order", which is what the composite seam has to pin (attachment -> copy ->
// sample -> composite) and what a counter cannot express.
enum class NullCommandKind : std::uint8_t {
    BeginRenderPass,
    EndRenderPass,
    BindPipeline,
    BindTexture,
    BindSampledTarget,
    BindStorageBuffer,
    Draw,
    CopyTargetRegion,
    Blit,
    Submit,
};

struct NullCommandStats {
    std::uint32_t begin_render_passes = 0;
    std::uint32_t end_render_passes = 0;
    std::uint32_t pipeline_binds = 0;
    std::uint32_t vertex_buffer_binds = 0;
    std::uint32_t texture_binds = 0;
    // bind_sampled_target() calls recorded, with one entry each in sampled_targets.
    std::uint32_t sampled_target_binds = 0;
    // bind_storage_buffer() calls recorded, with one entry each in storage_buffer_records (RHI
    // 1.12).
    std::uint32_t storage_buffer_binds = 0;
    std::uint32_t scissor_changes = 0;
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    // blit_target() calls recorded (the null backend resamples nothing; it counts the request).
    std::uint32_t blits = 0;
    // copy_target_region() calls recorded, with one entry each in target_region_copies.
    std::uint32_t region_copies = 0;
    std::vector<NullTargetRegionCopy> target_region_copies;
    std::vector<NullSampledTargetBind> sampled_targets;
    std::vector<NullStorageBufferBind> storage_buffer_records;
    // Ordered log of everything the list recorded.
    std::vector<NullCommandKind> command_order;
    // Sample count of the most recent begin_render_pass target (1 or 4).
    std::uint32_t last_render_pass_sample_count = 0;
    // One entry per draw, in recording order; `scissor` is the state active at draw time.
    std::vector<NullDrawRecord> draw_records;
};

// The null backend's pipeline record: what a creation request asked for, and nothing else, because
// the null backend rasterizes nothing. Both fields are the request as made, so a structural test
// can assert the state a real backend would have had to honour (the blend mode is what RHI 1.5's
// `BlendMode::DualSource` needs, since no pixel exists to inspect).
class NullPipeline final : public rhi::Pipeline {
public:
    explicit NullPipeline(const rhi::PipelineDesc& desc)
        : desc_(desc) {}

    [[nodiscard]] std::uint32_t sample_count() const override {
        return desc_.sample_count;
    }
    [[nodiscard]] rhi::BlendMode blend_mode() const {
        return desc_.blend;
    }
    // True when the pipeline's layout declares a read-only storage buffer (RHI 1.12, ADR-068), so
    // bind_storage_buffer can reject a bind the shader could never read.
    [[nodiscard]] bool declares_storage_buffer() const {
        return desc_.storage_buffer;
    }

private:
    rhi::PipelineDesc desc_;
};

class NullCommandList final : public rhi::CommandList {
public:
    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override;
    void end_render_pass() override;
    void bind_pipeline(const rhi::Pipeline& pipeline) override;
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override;
    void bind_texture_array(std::uint32_t slot,
                            const std::vector<const rhi::Texture*>& textures) override;
    void bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) override;
    void bind_storage_buffer(std::uint32_t slot, const rhi::Buffer& buffer) override;
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
    // Target of the open pass, marked as holding content when the pass closes.
    const rhi::RenderTarget* current_target_ = nullptr;
    // The pipeline bound last, so a bind that needs the pipeline's layout (bind_storage_buffer,
    // RHI 1.12) can reject a bind the shader could never read. Null until bind_pipeline.
    const NullPipeline* current_pipeline_ = nullptr;
    bool submitted_ = false;
    bool in_pass_ = false;
    std::uint32_t pass_sample_count_ = 1;
};

std::unique_ptr<rhi::Device> create_null_device(const rhi::DeviceDesc& desc);

} // namespace tessera::rendering::null
