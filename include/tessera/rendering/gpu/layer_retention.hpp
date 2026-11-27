// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/offscreen_target_pool.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::gpu {

// Retained layers (issue #617, docs/design/gpu/offscreen-layers.md section 5.6): the finished
// pixels of a static masked, filtered or faded group, kept between frames so a frame that repaints
// the group's area composites them instead of rendering the subtree, its mask and its filter again.
//
// A retained layer is one entry per owner (the scene node's id): the identity `key` its pixels
// were made under, the device-pixel `region` they cover, and an exact-size render target holding
// them as premultiplied colour, i.e. the layer after its filter and mask and before its opacity.
// The cache decides nothing about WHEN a key is stale: the caller (the paint pass and the renderer)
// folds every input into the key and calls invalidate() for a node that changed, so an entry that
// matches is, by construction, what a fresh render would produce.
//
// Memory is charged to the same OffscreenTargetPool as the live layer targets, so `max_bytes` of
// that pool bounds every byte of layer memory, and `RetainedLayerConfig::max_bytes` keeps a share
// of it for live layers: a retained layer never starves a layer that is being drawn. An evicted
// target is handed to the pool to wait for the frames in flight that may still read it, exactly
// like a regrown pool target; nothing is destroyed under the GPU and nothing blocks.
struct RetainedLayerConfig {
    // Entries held at once; 0 disables retention.
    std::uint32_t max_entries = 32;
    // Bytes of all retained targets together; 0 means only the pool's own cap applies.
    std::uint64_t max_bytes = 64ull * 1024 * 1024;
    // Frames of non-use after which an entry is released; 0 never.
    std::uint32_t idle_frames = 120;
};

struct RetainedLayerStats {
    std::uint32_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t hits = 0;        // layers composited from a retained target
    std::uint64_t records = 0;     // layers copied into a retained target
    std::uint64_t refused = 0;     // records skipped: entry cap, byte cap or pool cap
    std::uint64_t invalidated = 0; // entries dropped because their node changed or their key moved
    std::uint64_t evicted = 0;     // entries dropped for budget, idle time, resize or trim
};

class RetainedLayerCache {
public:
    RetainedLayerCache(OffscreenTargetPool* pool, RetainedLayerConfig config);

    RetainedLayerCache(const RetainedLayerCache&) = delete;
    RetainedLayerCache& operator=(const RetainedLayerCache&) = delete;

    [[nodiscard]] bool enabled() const {
        return pool_ != nullptr && config_.max_entries > 0;
    }

    // Starts a frame (after the pool's own begin_frame). A different frame extent or format drops
    // every entry: their regions are device pixels of the old frame. Idle entries are released.
    void begin_frame(std::uint64_t frame_index, rhi::Extent2D frame_extent);

    // What a hit hands the renderer.
    struct Hit {
        rhi::RenderTarget* target = nullptr;
        rhi::Scissor region; // device pixels of the frame the target holds, placed at its origin
    };
    // The ready entry of `owner` made under `key`, or an empty Hit. Marks it used this frame.
    [[nodiscard]] Hit find(std::uint64_t owner, std::uint64_t key);

    // Makes room for, creates and returns an exact-size target for `owner`'s next pixels, replacing
    // the owner's old entry. The entry is not ready (find() skips it) until commit(). Returns null,
    // counted in `refused`, when the entry cap, the byte cap or the pool's cap says no; entries
    // used this frame are never evicted for it, so a working set larger than the cap keeps what it
    // has instead of thrashing.
    [[nodiscard]] rhi::RenderTarget* reserve(std::uint64_t owner, std::uint64_t key,
                                             const rhi::Scissor& region);
    // The copy into the reserved target is recorded: find() may now return it.
    void commit(std::uint64_t owner);
    // The layer that reserved a target could not finish (a fallback ran): drop the reservation.
    void abandon(std::uint64_t owner);
    // The node changed (or its key moved): drop its entry.
    void invalidate(std::uint64_t owner);
    // Drops everything. `all_complete` is the caller's statement that no frame in flight reads a
    // retained target, so they are destroyed at once; otherwise they wait for their frames.
    void trim(bool all_complete);

    [[nodiscard]] RetainedLayerStats stats() const;

private:
    struct Entry {
        std::uint64_t owner = 0;
        std::uint64_t key = 0;
        std::unique_ptr<rhi::RenderTarget> target;
        rhi::Scissor region;
        std::uint64_t bytes = 0;
        std::uint64_t last_used = 0;
        bool ready = false;
    };

    [[nodiscard]] Entry* entry_of(std::uint64_t owner);
    void release(std::size_t index, bool immediate);
    // Evicts the least recently used entry not used this frame; false when there is none.
    [[nodiscard]] bool evict_one();

    OffscreenTargetPool* pool_ = nullptr;
    RetainedLayerConfig config_;
    std::vector<Entry> entries_;
    std::uint64_t frame_index_ = 0;
    std::uint64_t bytes_ = 0;
    rhi::Extent2D frame_extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    RetainedLayerStats stats_;
};

// The renderer's side of retention, kept here so batched2d_renderer.hpp stays small.
//
// retain_layer() arms a `LayerRetainPending`, the very next push_layer() consumes it: a hit makes
// that layer composite a retained target (its content is never drawn), a miss that may record makes
// it render its full box (ignoring the frame's damage, because the pixels must serve any later
// damage) and copy the finished result into a reserved target when it pops.
struct LayerRetainPending {
    bool hit = false;
    bool record = false;
    std::uint64_t owner = 0;
    std::uint64_t key = 0;
    RetainedLayerCache::Hit entry;
};

// What one open layer remembers (a member of the renderer's LayerFrame).
struct LayerRetainFrame {
    bool retained = false;         // composites `target`, draws nothing
    bool recording = false;        // copies its result into `target` (reserved) at pop_layer()
    bool suspended_damage = false; // this layer turned the frame's damage off while it renders
    std::uint64_t owner = 0;
    std::uint32_t degraded_mark = 0; // renderer_degradations() when the layer opened
    rhi::RenderTarget* target = nullptr;
    rhi::Scissor placement; // retained: the frame region `target` holds, where its origin lands
};

struct LayerRetainState {
    std::unique_ptr<RetainedLayerCache> cache;
    LayerRetainPending pending;
    // The frame's damage while a recording layer renders its whole box.
    std::vector<rhi::Scissor> saved_regions;
    rhi::Scissor saved_bounds;
    bool damage_suspended = false;
    // Events that make the pixels of an open recording layer unfit to keep (an image still
    // loading, a glass node that could not run): reported by the sink through
    // Batched2DRenderer::note_layer_degraded().
    std::uint32_t degraded = 0;
};

} // namespace tessera::rendering::gpu
