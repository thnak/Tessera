// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <expected>
#include <memory>

namespace tessera::rendering::path {

struct PathTextureCacheConfig {
    // Bounded LRU. Both bounds are enforced exactly: the cache never holds more entries than
    // `max_entries` nor more pixel bytes than `max_bytes` (RGBA8: width*height*4 per entry), except
    // transiently while one entry is being created. A single raster larger than `max_bytes` is
    // rejected with `entry_too_large` instead of flushing the cache on every request.
    std::uint32_t max_entries = 64;
    std::uint64_t max_bytes = 32ULL * 1024ULL * 1024ULL; // 32 MiB default bound

    // Upload format. Use R8G8B8A8Unorm when drawing into a UNORM target (the deterministic
    // golden path keeps the raster bytes unchanged) and R8G8B8A8Srgb when drawing into an sRGB
    // target (D2): sampling decodes to linear and the sRGB attachment re-encodes, so the stored
    // encoded bytes round-trip instead of being encoded twice.
    rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    rhi::Filter filter = rhi::Filter::Linear;
    rhi::AddressMode address = rhi::AddressMode::ClampToEdge;
};

struct CachedPathTexture {
    rhi::Texture* texture = nullptr;
    // Size of the uploaded raster (`key.width * scale` x `key.height * scale`, rounded up).
    rhi::Extent2D extent;
};

enum class PathTextureError : std::uint8_t {
    invalid_request = 0, // zero size/scale, scaled extent overflow, invalid path or paint
    entry_too_large,     // one raster exceeds `max_bytes`, so it can never be cached
    device_lost,         // the device reported Lost; the cache was invalidated
    rasterize_failed,    // the PathRasterizer backend failed
    upload_failed,       // the device could not create the texture
};

// Uploads PathRasterizer output into RHI textures and keeps them in a bounded LRU.
//
// Contract:
// - `acquire()` returns a cached texture on a hit (no rasterizer call, no device call, no
//   allocation) and rasterizes + uploads once on a miss.
// - The returned pointer is stable until that entry is evicted, `invalidate_all()` runs, or a
//   device loss is observed. Do not hold it across those events.
// - Eviction is deterministic: least-recently-used first (a hit moves an entry to the front, a
//   miss appends at the front), so a fixed request sequence always evicts the same entries.
// - Device loss (D7): the cache polls `Device::status()`. On `Lost` it drops every texture through
//   the RHI and returns `device_lost`; after the device returns to `Ready` the observed
//   `generation()` bumps and the next acquire re-rasterizes. Callers that see the loss themselves
//   should call `notify_device_lost()` before `Device::recover()`, because recovery refuses while
//   lost-generation resources are still alive.
// - Not thread-safe; one instance per rendering thread, like the rest of the renderer.
class PathTextureCache {
public:
    struct Stats {
        std::uint64_t hits = 0;           // acquire() served from the cache
        std::uint64_t misses = 0;         // acquire() with no entry (including failed uploads)
        std::uint64_t evictions = 0;      // entries dropped to stay inside the bounds
        std::uint64_t rasterizations = 0; // PathRasterizer calls
        std::uint64_t uploads = 0;        // rhi::Texture creations
        std::uint64_t invalidations = 0;  // loss/notify/invalidate_all events
        std::uint32_t entries = 0;
        std::uint64_t bytes = 0; // sum of entry pixel bytes
    };

    PathTextureCache(rhi::Device& device, PathRasterizer& rasterizer,
                     PathTextureCacheConfig config = {});
    ~PathTextureCache();

    PathTextureCache(const PathTextureCache&) = delete;
    PathTextureCache& operator=(const PathTextureCache&) = delete;
    PathTextureCache(PathTextureCache&&) = delete;
    PathTextureCache& operator=(PathTextureCache&&) = delete;

    // Serves `key` from the cache, or rasterizes `shape` at the key's scale and uploads it. `shape`
    // is ignored on a hit, so callers may pass a cheap placeholder then; use
    // `make_path_texture_key()` to build keys so the signature and geometry stay consistent.
    [[nodiscard]] std::expected<CachedPathTexture, PathTextureError>
    acquire(const PathTextureKey& key, const PathShape& shape);

    // Drops every entry (textures are destroyed through the RHI). Call before Device::recover().
    void invalidate_all();

    // Explicit loss hook for callers that observe DeviceStatus::Lost outside acquire(). Drops every
    // entry and marks the current generation lost; `generation()` bumps once Ready is observed.
    void notify_device_lost();

    // Observed device generation: starts at 0 and increments on every Lost -> Ready transition.
    [[nodiscard]] std::uint64_t generation() const;

    [[nodiscard]] std::uint32_t entry_count() const;
    [[nodiscard]] std::uint64_t byte_count() const;
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::rendering::path
