// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <expected>
#include <list>
#include <memory>
#include <span>
#include <unordered_map>

namespace tessera::rendering::image {

struct ImageDecodeCacheConfig {
    // Bounded LRU over decoded RGBA8 buffers. A single image larger than `max_bytes` is decoded but
    // not retained (it still returns a value for that call), so it cannot flush the whole cache.
    std::uint32_t max_entries = 64;
    std::uint64_t max_bytes = 32ULL * 1024ULL * 1024ULL;
};

// Decodes embedded image bytes once per content hash and keeps the RGBA8 result in a bounded LRU.
//
// Contract:
// - `acquire()` decodes on a miss and returns a stable pointer to the cached decode; hits do no
//   decode, no allocation and no filesystem access.
// - The returned pointer is stable until that entry is evicted or `clear()` runs.
// - Eviction is deterministic (least-recently-used first).
// - Keys are the FNV-1a content hash of the encoded bytes, matching the vector path's identity
//   rule, so a changed asset is automatically a new key.
// - Decoding is a pure function of bytes + limits; failures are values and are never cached.
// - Not thread-safe; one instance per rendering thread.
class ImageDecodeCache {
public:
    struct Stats {
        std::uint64_t hits = 0;      // acquire() served from the cache
        std::uint64_t misses = 0;    // acquire() with no entry (including failed decodes)
        std::uint64_t decodes = 0;   // successful stb_image decodes
        std::uint64_t evictions = 0; // entries dropped to stay inside the bounds
        std::uint32_t entries = 0;
        std::uint64_t bytes = 0; // sum of decoded RGBA bytes
    };

    explicit ImageDecodeCache(ImageDecodeCacheConfig config = {});
    ~ImageDecodeCache();

    ImageDecodeCache(const ImageDecodeCache&) = delete;
    ImageDecodeCache& operator=(const ImageDecodeCache&) = delete;
    ImageDecodeCache(ImageDecodeCache&&) = delete;
    ImageDecodeCache& operator=(ImageDecodeCache&&) = delete;

    // Returns a shared, immutable decode: callers may hold it across cache eviction. Oversized
    // images (one decode above `max_bytes`) are returned but not retained.
    [[nodiscard]] std::expected<std::shared_ptr<const DecodedImage>, ImageError>
    acquire(std::span<const std::uint8_t> bytes, ImageLimits limits = {});

    void clear();

    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::rendering::image
