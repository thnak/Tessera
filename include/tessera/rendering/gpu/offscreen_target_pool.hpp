// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::gpu {

// Bounded pool of offscreen render targets for the isolated-layer seam
// (docs/design/gpu/offscreen-layers.md, issue #242). One target is the backdrop copy that #244
// blurs; the same pool later backs a full isolated layer (#227) and shared-element snapshots, so a
// target is a plain render target the caller can render into, copy into, or read back - not a
// blur-only resource.
//
// Retention: a target acquired during frame F is referenced by F's command list, so the pool hands
// it out again only once `frames_in_flight` frame slots have rotated - the renderer's documented
// "frame F is complete" point. It therefore never grows past `max_targets`, never destroys a target
// the GPU may still read, and reuses one target for a steady-state screen (the 300-frame test in
// tests/gpu/batched2d/renderer_seam_tests.cpp keeps target_count() at one).
//
// Sizing: a free target grows to the largest extent requested on it and keeps that high-water
// extent for later frames. A target used by the current frame is never recreated mid-frame,
// because a recorded copy may still reference its image.
struct OffscreenTargetPoolConfig {
    // Hard cap on live targets. This is `glassMaxBlurTargets` from docs/specs/glass-v1.md section 5
    // (default 4); 0 disables the pool.
    std::uint32_t max_targets = 4;
};

class OffscreenTargetPool {
public:
    explicit OffscreenTargetPool(rhi::Device& device, OffscreenTargetPoolConfig config = {});
    ~OffscreenTargetPool();

    OffscreenTargetPool(const OffscreenTargetPool&) = delete;
    OffscreenTargetPool& operator=(const OffscreenTargetPool&) = delete;

    // Starts a frame. Call once per frame before acquire(), from the renderer's frame begin (which
    // already enforces "frame F is complete when its slot is reused"). `frame_index` must increase
    // monotonically; `format` is the frame target's format, which every pooled target uses.
    void begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                     rhi::TextureFormat format);

    // A target at least `extent` in size, for use during the current frame. Reuses a free target
    // (growing it if needed) or creates one up to the cap; returns nullptr when every live target
    // is still referenced by this frame or by frames in flight, so the caller paints that frame
    // without the effect instead of blocking or growing without bound. Consecutive calls in one
    // frame return different targets.
    [[nodiscard]] rhi::RenderTarget* acquire(rhi::Extent2D extent);

    [[nodiscard]] std::uint32_t target_count() const {
        return static_cast<std::uint32_t>(entries_.size());
    }
    // Live extent of one pooled target; zero-sized for an out-of-range index.
    [[nodiscard]] rhi::Extent2D target_extent(std::uint32_t index) const;
    // Lifetime counters for structural tests and diagnostics; they never reset.
    [[nodiscard]] std::uint64_t created() const {
        return created_;
    }
    [[nodiscard]] std::uint64_t reused() const {
        return reused_;
    }

private:
    struct Entry {
        std::unique_ptr<rhi::RenderTarget> target;
        std::uint64_t last_used_frame = 0; // 0 = never used
        bool used_this_frame = false;
    };

    [[nodiscard]] bool free_for_next_frame(const Entry& entry) const;
    [[nodiscard]] std::unique_ptr<rhi::RenderTarget> create_target(rhi::Extent2D extent) const;
    static void grow_entry(Entry& entry, rhi::Extent2D extent, rhi::TextureFormat format,
                           rhi::Device& device, std::uint64_t& created);

    rhi::Device* device_ = nullptr;
    OffscreenTargetPoolConfig config_;
    std::vector<Entry> entries_;
    std::uint64_t frame_index_ = 0;
    std::uint32_t frames_in_flight_ = 1;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    std::uint64_t created_ = 0;
    std::uint64_t reused_ = 0;
};

} // namespace tessera::rendering::gpu
