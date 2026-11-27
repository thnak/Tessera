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
// Retention has two rules, by who ends the use:
//  - A target the caller release()s is free at once, in this frame and in the next. The command
//    list is sequential and the queue executes frames in submission order, so the backend only has
//    to order the new pass after the old reads, which it does from the target's tracked layout
//    (Vulkan records that barrier in begin_render_pass; its first scope reaches back across
//    submissions on the queue). So the bound that matters is the PEAK number of live targets, not
//    the number of layers and not the frames in flight: twelve sibling masked layers need two
//    targets, and so does every later frame.
//  - A target that is never released (acquire() then nothing) is referenced by its frame's command
//    list, so it is handed out again only once `frames_in_flight` frame slots have rotated - the
//    renderer's documented "frame F is complete" point.
// A target is only ever DESTROYED (grown, trimmed) after that rotation, because the image must
// outlive the commands that read it.
//
// Sizing: a free target grows to the largest extent requested on it and keeps that high-water
// extent. A free target that still has a frame in flight and is too small is retired instead of
// destroyed: it stays alive, out of the pool, until its frame has rotated, and a new one of the
// larger size takes its slot (a live window resize would otherwise starve the pool for
// `frames_in_flight` frames). Retired targets count against the byte cap.
//
// Budget: two hard caps stop growth, the count (`max_targets`) and the bytes (`max_bytes`), and
// a request over either returns nullptr and is counted (`exhausted`), so a caller paints without
// the effect instead of blocking or growing without bound.
//
// Idle: a target no frame has used for `idle_trim_frames` frames is destroyed at the next
// begin_frame(), and trim() destroys every target an idle caller knows the GPU has finished with,
// so a scene that stops using layers (or stops drawing at all) holds no extra GPU memory.
struct OffscreenTargetPoolConfig {
    // Hard cap on live targets. This is `glassMaxBlurTargets` from docs/specs/glass-v1.md section 5
    // (default 4); 0 disables the pool.
    std::uint32_t max_targets = 4;
    // Hard cap on the bytes of all pooled targets together (extent x texel size, 1 sample); 0 means
    // no byte cap. The RHI reports no heap budget, so this is a fixed engine default sized from the
    // frame (see docs/design/gpu/offscreen-layers.md section 5e).
    std::uint64_t max_bytes = 0;
    // Frames of non-use after which a free target is destroyed; 0 never trims. Raised to
    // frames_in_flight when lower, so a target the GPU may still read is never destroyed.
    std::uint32_t idle_trim_frames = 0;
};

// Observable pool state, for tests, the renderer's stats and the diagnostics reporter.
struct OffscreenPoolStats {
    std::uint32_t targets = 0;         // live pooled targets (retired ones excluded)
    std::uint32_t live = 0;            // handed out and not yet released, this frame
    std::uint32_t peak_live = 0;       // highest `live` this frame
    std::uint32_t peak_live_max = 0;   // highest `live` of any frame since creation
    std::uint64_t bytes = 0;           // bytes of the live and retired targets
    std::uint64_t peak_bytes = 0;      // highest `bytes` since creation
    std::uint32_t exhausted = 0;       // requests refused by a cap, this frame
    std::uint64_t exhausted_total = 0; // requests refused by a cap since creation
    std::uint64_t trimmed_total = 0;   // targets destroyed idle since creation
    std::uint32_t max_targets = 0;
    std::uint64_t max_bytes = 0;
};

class OffscreenTargetPool {
public:
    explicit OffscreenTargetPool(rhi::Device& device, OffscreenTargetPoolConfig config = {});
    ~OffscreenTargetPool();

    OffscreenTargetPool(const OffscreenTargetPool&) = delete;
    OffscreenTargetPool& operator=(const OffscreenTargetPool&) = delete;

    // Starts a frame. Call once per frame before acquire(), from the renderer's frame begin (which
    // already enforces "frame F is complete when its slot is reused"). `frame_index` must increase
    // monotonically; `format` is the frame target's format, which every pooled target uses. Resets
    // the per-frame counters and trims idle targets.
    void begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                     rhi::TextureFormat format);

    // A target at least `extent` in size, for use during the current frame. Reuses a free target
    // (growing it if needed) or creates one up to the cap; returns nullptr when every live target
    // is still referenced by this frame or by frames in flight, or a cap would be exceeded, so the
    // caller paints that frame without the effect instead of blocking or growing without bound.
    // Calls in one frame return different targets until one is release()d.
    [[nodiscard]] rhi::RenderTarget* acquire(rhi::Extent2D extent);
    // Returns `target` (from this frame's acquire()) to the pool: the caller has recorded its last
    // use, so a later acquire() in the same frame may hand it out again. A pointer the pool does
    // not own, or one already released, is ignored.
    void release(const rhi::RenderTarget* target);
    // Destroys every free target. `all_complete` is the caller's statement that no frame in flight
    // references any pooled target (the window drained its work), which lets it also free targets
    // of the most recent frames; false trims only those idle past `frames_in_flight`.
    void trim(bool all_complete);

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
    [[nodiscard]] OffscreenPoolStats stats() const;

    // Retained layers (layer_retention.hpp) own their targets but charge the bytes to this pool's
    // cap, so one number bounds every byte of layer memory. charge() reserves `bytes` when the
    // total stays within `max_bytes` (false: refused, nothing charged); refund() gives them back
    // once the image is destroyed. retire() adopts a target a frame in flight may still read: it is
    // kept alive, its bytes still charged, until `last_used_frame` has rotated (like a regrown
    // target), then destroyed with its bytes refunded.
    [[nodiscard]] bool charge(std::uint64_t bytes);
    void refund(std::uint64_t bytes);
    void retire(std::unique_ptr<rhi::RenderTarget> target, std::uint64_t last_used_frame,
                std::uint64_t bytes);
    // Bytes of a single-sampled target of `extent` in the current frame format.
    [[nodiscard]] std::uint64_t bytes_for(rhi::Extent2D extent) const {
        return extent_bytes(extent);
    }
    [[nodiscard]] rhi::TextureFormat format() const {
        return format_;
    }
    [[nodiscard]] rhi::Device& device() const {
        return *device_;
    }

private:
    struct Entry {
        std::unique_ptr<rhi::RenderTarget> target;
        std::uint64_t last_used_frame = 0; // 0 = never used
        // Acquired and not yet released: unavailable until release(), or until its frame rotates.
        bool held = false;
        // Acquired in the current frame (the `live` counter's membership).
        bool used_this_frame = false;
        std::uint64_t bytes = 0;
    };
    // A target kept alive after its slot was regrown, until the frame that last read it rotates.
    struct Retired {
        std::unique_ptr<rhi::RenderTarget> target;
        std::uint64_t last_used_frame = 0;
        std::uint64_t bytes = 0;
    };

    // True when the frames that could still read the entry's target have all completed.
    [[nodiscard]] bool rotated(std::uint64_t last_used_frame) const;
    // True when acquire() may hand the entry out: released, or held by a frame that completed.
    [[nodiscard]] bool available(const Entry& entry) const;
    [[nodiscard]] std::uint64_t extent_bytes(rhi::Extent2D extent) const;
    void note_live(Entry& entry);
    void erase_entry(std::size_t index);
    void drop_retired(bool all);
    [[nodiscard]] std::unique_ptr<rhi::RenderTarget> create_target(rhi::Extent2D extent) const;

    rhi::Device* device_ = nullptr;
    OffscreenTargetPoolConfig config_;
    std::vector<Entry> entries_;
    std::vector<Retired> retired_;
    std::uint64_t frame_index_ = 0;
    std::uint32_t frames_in_flight_ = 1;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    std::uint64_t created_ = 0;
    std::uint64_t reused_ = 0;
    std::uint64_t bytes_ = 0;
    OffscreenPoolStats stats_;
};

} // namespace tessera::rendering::gpu
