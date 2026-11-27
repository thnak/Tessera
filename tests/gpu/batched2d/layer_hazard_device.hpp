// SPDX-License-Identifier: MIT
//
// A null-backend device whose command lists check the recycling contract of the offscreen pool
// (docs/design/gpu/offscreen-layers.md section 5e). The null backend rasterizes nothing, so it
// cannot show that a pooled target handed out again inside a frame still holds what a later pass
// needs. This wrapper tracks the one invariant that makes recycling correct and that an early
// release() breaks:
//
//  - a target cleared by a pass holds content that a later pass reads (it is composited, masked
//    with, or blurred from); that content must be sampled before the target is cleared again.
//    Clearing a target whose last write nobody sampled loses it (`lost_writes`).
//  - the Vulkan backend refuses a pass on a target that is still the bound sampled texture; the
//    wrapper counts the same sequence (`sampled_pass_conflicts`) so a recycled target is never
//    cleared straight after being sampled without the sampling pass having ended.
//
// The frame target is cleared every frame and never sampled; callers pass it to ignore().

#pragma once

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace layer_hazard {

namespace rhi = tessera::rendering::rhi;
namespace null = tessera::rendering::null;

struct Log {
    std::set<const rhi::RenderTarget*> ignored;
    std::set<const rhi::RenderTarget*> unsampled; // cleared, content not yet sampled
    std::set<const rhi::RenderTarget*> used;      // every non-ignored target a pass drew into
    const rhi::RenderTarget* sampled_pending = nullptr;
    std::uint32_t lost_writes = 0;
    std::uint32_t sampled_pass_conflicts = 0;
    std::uint32_t clears = 0;

    void ignore(const rhi::RenderTarget* target) {
        ignored.insert(target);
    }
    // Targets whose cleared content was never sampled by the end of a frame.
    [[nodiscard]] std::size_t unsampled_targets() const {
        return unsampled.size();
    }
};

class CommandList final : public rhi::CommandList {
public:
    CommandList(std::unique_ptr<rhi::CommandList> inner, Log& log)
        : inner_(std::move(inner)),
          log_(log) {}

    [[nodiscard]] const null::NullCommandStats& stats() const {
        return static_cast<const null::NullCommandList*>(inner_.get())->stats();
    }

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override {
        if (log_.ignored.count(&target) == 0) {
            log_.used.insert(&target);
            if (log_.sampled_pending == &target) {
                ++log_.sampled_pass_conflicts;
            }
            if (desc.load_op == rhi::LoadOp::Clear) {
                ++log_.clears;
                if (log_.unsampled.count(&target) != 0) {
                    ++log_.lost_writes;
                }
                log_.unsampled.insert(&target);
            }
        }
        inner_->begin_render_pass(target, desc);
    }
    void end_render_pass() override {
        log_.sampled_pending = nullptr;
        inner_->end_render_pass();
    }
    void bind_pipeline(const rhi::Pipeline& pipeline) override {
        inner_->bind_pipeline(pipeline);
    }
    void bind_vertex_buffer(const rhi::Buffer& buffer) override {
        inner_->bind_vertex_buffer(buffer);
    }
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override {
        log_.sampled_pending = nullptr;
        inner_->bind_texture(slot, texture);
    }
    void bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) override {
        log_.unsampled.erase(&target);
        log_.sampled_pending = &target;
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
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex) override {
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

private:
    std::unique_ptr<rhi::CommandList> inner_;
    Log& log_;
};

class Device final : public rhi::Device {
public:
    explicit Device(Log& log)
        : inner_(null::create_null_device(rhi::DeviceDesc{})),
          log_(log) {}

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
        return std::make_unique<CommandList>(inner_->create_command_list(), log_);
    }
    std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override {
        return inner_->create_swapchain(desc);
    }
    void wait_idle() override {
        inner_->wait_idle();
    }

private:
    std::unique_ptr<rhi::Device> inner_;
    Log& log_;
};

} // namespace layer_hazard
