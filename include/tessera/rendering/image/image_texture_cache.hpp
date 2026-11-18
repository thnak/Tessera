// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <expected>
#include <memory>

namespace tessera::rendering::image {

// Texture identity for one decoded image at one raster size and sampling filter. Images have no
// scale-relative geometry, so the raster extent is explicit: the decoded pixels are resampled
// (deterministic nearest) to this extent before upload, and the quad draws the texture into the
// node content box.
//
// The filter is part of the identity because RHI sampler state is fixed at texture creation: an
// image drawn once at an aligned 1:1 position (nearest) and once scaled (linear) needs one texture
// per filter. The cache limits below are per entry, so caching both variants of one image doubles
// its entries and can evict another image's; that is the documented cost of a per-draw choice.
struct ImageTextureKey {
    std::uint64_t content_hash = 0;
    std::uint32_t width = 0; // raster extent in device pixels
    std::uint32_t height = 0;
    rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    // Nearest keeps the deterministic reference exact and matches CpuCanvas::blit_texture; linear
    // is the choice for a draw that is not aligned 1:1 with the uploaded raster (S8). The cache
    // never overrides it: the key's filter is the texture's sampler state.
    rhi::Filter filter = rhi::Filter::Nearest;
    // Mip levels of the uploaded texture (RHI 1.6, issue #491): 1 for a single level, more for a
    // chain built from the raster at creation. It is part of the identity because the sampler's LOD
    // range is fixed at creation, like the filter; a draw that minifies the raster asks for the
    // full chain (`ui::paint::image_levels_for_draw`), every other draw for one level.
    std::uint32_t levels = 1;

    [[nodiscard]] bool operator==(const ImageTextureKey&) const noexcept = default;
};

struct ImageTextureCacheConfig {
    std::uint32_t max_entries = 64;
    std::uint64_t max_bytes = 64ULL * 1024ULL * 1024ULL;
    // D2 rule: UNORM for UNORM targets (the authored encoded bytes are kept; deterministic
    // goldens), sRGB for sRGB targets (sampling decodes to linear and the attachment re-encodes, so
    // the authored bytes round-trip instead of being encoded twice). PNG/JPEG are authored sRGB, so
    // the image path must use the target's color space; this is the same rule as the vector path.
    // The per-entry sampling filter lives on `ImageTextureKey`, not here: it is chosen per draw.
    rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    rhi::AddressMode address = rhi::AddressMode::ClampToEdge;
};

// The texture pointer is valid until end_frame() after the entry is evicted (or until
// invalidate_all()/device loss): eviction parks the texture rather than destroying it, so draws
// already recorded this frame stay valid. The parking lot is bounded by the cache limits; past that
// the oldest parked texture is destroyed early (Stats::forced_releases).
struct CachedImageTexture {
    rhi::Texture* texture = nullptr;
    rhi::Extent2D extent; // uploaded raster extent (== key.width/height)
};

enum class ImageTextureError : std::uint8_t {
    invalid_request = 0, // zero extent, scaled overflow, unset content hash, bad format
    entry_too_large,     // one raster exceeds `max_bytes`, so it can never be cached
    device_lost,         // the device reported Lost; the cache was invalidated
    upload_failed,       // the device could not create the texture
};

// Uploads decoded images into RHI textures and keeps them in a bounded LRU, mirroring
// PathTextureCache's contract (stable handles, deterministic eviction, D7 generation handling, no
// threading). A key's raster extent resamples the decoded pixels with deterministic nearest
// sampling, so the uploaded bytes depend only on (content, extent, format).
class ImageTextureCache {
public:
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t uploads = 0;         // rhi::Texture creations
        std::uint64_t invalidations = 0;   // loss/notify/invalidate_all events
        std::uint64_t forced_releases = 0; // parked textures destroyed early to stay bounded
        std::uint32_t entries = 0;
        std::uint32_t retired = 0; // evicted textures parked until end_frame()
        std::uint64_t bytes = 0;
    };

    ImageTextureCache(rhi::Device& device, ImageTextureCacheConfig config = {});
    ~ImageTextureCache();

    ImageTextureCache(const ImageTextureCache&) = delete;
    ImageTextureCache& operator=(const ImageTextureCache&) = delete;
    ImageTextureCache(ImageTextureCache&&) = delete;
    ImageTextureCache& operator=(ImageTextureCache&&) = delete;

    [[nodiscard]] std::expected<CachedImageTexture, ImageTextureError>
    acquire(const DecodedImage& image, const ImageTextureKey& key);

    // Whether the device can build a mip chain for the cache's texture format. A caller asks before
    // it puts `levels > 1` in a key; a device that cannot is asked for a single level instead.
    [[nodiscard]] bool supports_mipmaps() const;

    // Frame boundary: destroys textures that were evicted while their draws could still be pending.
    // Call after the frame's commands that used this cache's textures have been submitted and are
    // no longer in flight. Eviction parks the texture (bounded by the cache limits) instead of
    // destroying it, so a frame may draw more distinct keys than `max_entries` safely as long as
    // end_frame() runs between frames.
    void end_frame();

    // Drops every entry (textures are destroyed through the RHI). Call before Device::recover().
    void invalidate_all();

    // Explicit loss hook for callers that observe DeviceStatus::Lost outside acquire().
    void notify_device_lost();

    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::uint32_t entry_count() const;
    [[nodiscard]] std::uint64_t byte_count() const;
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::rendering::image
