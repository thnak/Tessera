// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera::rendering::path {
namespace {

constexpr std::uint32_t kBytesPerPixel = 4;
// Hard guard against degenerate requests. A 16384-pixel edge keeps width/height products and
// `uint32_t` row pitches well inside range; anything larger is a caller bug, not a cache policy.
constexpr std::uint32_t kMaxRasterExtent = 16384;

// FNV-1a over the key fields, mixed explicitly so struct padding never enters the hash (the hash
// only affects bucket placement, but a padding-dependent hash would be non-deterministic).
[[nodiscard]] std::size_t hash_key(const PathTextureKey& key) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    mix(key.path_id);
    mix(key.path_version);
    mix(key.width);
    mix(key.scale_milli);
    mix(key.height);
    mix(static_cast<std::uint64_t>(key.fill_rule));
    mix(key.fill.present ? 1U : 0U);
    mix(key.fill.rgba);
    mix(key.stroke.present ? 1U : 0U);
    mix(key.stroke.rgba);
    mix(key.stroke.width_milli);
    mix(key.stroke.miter_milli);
    mix(static_cast<std::uint64_t>(key.stroke.cap));
    mix(static_cast<std::uint64_t>(key.stroke.join));
    mix(key.document ? 1U : 0U);
    return static_cast<std::size_t>(hash);
}

struct PathTextureKeyHash {
    [[nodiscard]] std::size_t operator()(const PathTextureKey& key) const noexcept {
        return hash_key(key);
    }
};

[[nodiscard]] PathTextureError map_raster_error(RasterError error) noexcept {
    switch (error) {
    case RasterError::invalid_target:
    case RasterError::invalid_path:
    case RasterError::invalid_paint:
        return PathTextureError::invalid_request;
    case RasterError::unsupported_backend:
    case RasterError::backend_failure:
        break;
    }
    return PathTextureError::rasterize_failed;
}

[[nodiscard]] std::uint32_t ceil_scale(std::uint32_t size, std::uint32_t scale_milli) noexcept {
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(size) * static_cast<std::uint64_t>(scale_milli);
    return static_cast<std::uint32_t>((scaled + 999ULL) / 1000ULL);
}

} // namespace

struct PathTextureCache::Impl {
    struct Entry {
        PathTextureKey key;
        std::unique_ptr<rhi::Texture> texture;
        rhi::Extent2D extent;
        std::uint64_t bytes = 0;
    };

    Impl(rhi::Device& target_device, PathRasterizer& path_rasterizer,
         PathTextureCacheConfig cache_config)
        : device(target_device),
          rasterizer(path_rasterizer),
          config(cache_config) {
        config.max_entries = std::max(1U, config.max_entries);
        config.max_bytes = std::max<std::uint64_t>(kBytesPerPixel, config.max_bytes);
    }

    // D7 health poll. A Lost -> Ready transition is a new texture generation: every texture created
    // before the loss is invalid, so the cache must be empty when rendering resumes.
    [[nodiscard]] bool observe_device() {
        const rhi::DeviceStatus status = device.status();
        if (status == rhi::DeviceStatus::Lost) {
            if (!device_lost) {
                device_lost = true;
                clear_entries();
                ++stats.invalidations;
            }
        } else if (device_lost) {
            device_lost = false;
            ++generation;
            clear_entries();
            ++stats.invalidations;
        }
        return device_lost;
    }

    void clear_entries() {
        entries.clear(); // unique_ptr destruction is the only release path: RHI owns the memory
        index.clear();
        bytes = 0;
    }

    void evict_until_fits(std::uint64_t incoming_bytes) {
        while (!entries.empty() && (entries.size() >= config.max_entries ||
                                    bytes + incoming_bytes > config.max_bytes)) {
            const Entry& victim = entries.back();
            bytes -= victim.bytes;
            index.erase(victim.key);
            entries.pop_back();
            ++stats.evictions;
        }
    }

    [[nodiscard]] std::expected<CachedPathTexture, PathTextureError>
    rasterize_and_upload(const PathTextureKey& key, std::span<const PathShape> shapes,
                         rhi::Extent2D extent) {
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(extent.width) * kBytesPerPixel;
        const std::uint64_t entry_bytes = row_bytes * extent.height;

        // Geometry and stroke scale together, so drawing the larger texture down into the logical
        // box restores the authored stroke width. A whole document is scaled as a unit.
        const float scale = static_cast<float>(key.scale_milli) / 1000.0f;
        std::vector<PathShape> scaled_storage;
        std::span<const PathShape> to_rasterize = shapes;
        if (scale != 1.0f) {
            scaled_storage.assign(shapes.begin(), shapes.end());
            for (PathShape& scaled : scaled_storage) {
                for (PathSegment& segment : scaled.path.segments) {
                    segment.p0 = {segment.p0.x * scale, segment.p0.y * scale};
                    segment.p1 = {segment.p1.x * scale, segment.p1.y * scale};
                    segment.p2 = {segment.p2.x * scale, segment.p2.y * scale};
                }
                if (scaled.paint.stroke.has_value()) {
                    scaled.paint.stroke->width *= scale;
                }
            }
            to_rasterize = std::span<const PathShape>(scaled_storage);
        }

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(entry_bytes), 0);
        RasterTarget target{std::span<std::uint8_t>(pixels.data(), pixels.size()), extent.width,
                            extent.height, static_cast<std::uint32_t>(row_bytes)};

        ++stats.rasterizations;
        const auto raster = rasterizer.rasterize(to_rasterize, target);
        if (!raster.has_value()) {
            return std::unexpected(map_raster_error(raster.error()));
        }

        rhi::TextureDesc desc;
        desc.extent = extent;
        desc.format = config.format;
        desc.pixels = pixels.data();
        desc.row_pitch = static_cast<std::size_t>(row_bytes);
        desc.filter = config.filter;
        desc.address = config.address;

        std::unique_ptr<rhi::Texture> texture;
        try {
            texture = device.create_texture(desc);
        } catch (const std::exception&) {
            return std::unexpected(PathTextureError::upload_failed);
        }
        if (!texture) {
            return std::unexpected(PathTextureError::upload_failed);
        }
        ++stats.uploads;

        evict_until_fits(entry_bytes);
        const auto inserted =
            entries.emplace(entries.begin(), Entry{key, std::move(texture), extent, entry_bytes});
        index.emplace(key, inserted);
        bytes += entry_bytes;
        return CachedPathTexture{inserted->texture.get(), inserted->extent};
    }

    rhi::Device& device;
    PathRasterizer& rasterizer;
    PathTextureCacheConfig config;

    // Front = most recently used; eviction always takes the back, so the order is total and
    // deterministic for a fixed request sequence.
    std::list<Entry> entries;
    std::unordered_map<PathTextureKey, std::list<Entry>::iterator, PathTextureKeyHash> index;
    std::uint64_t bytes = 0;
    std::uint64_t generation = 0;
    bool device_lost = false;
    Stats stats;
};

PathTextureCache::PathTextureCache(rhi::Device& device, PathRasterizer& rasterizer,
                                   PathTextureCacheConfig config)
    : impl_(std::make_unique<Impl>(device, rasterizer, config)) {}

PathTextureCache::~PathTextureCache() = default;

std::expected<CachedPathTexture, PathTextureError>
PathTextureCache::acquire(const PathTextureKey& key, const PathShape& shape) {
    return acquire(key, std::span<const PathShape>(&shape, 1));
}

std::expected<CachedPathTexture, PathTextureError>
PathTextureCache::acquire(const PathTextureKey& key, std::span<const PathShape> shapes) {
    if (impl_->observe_device()) {
        return std::unexpected(PathTextureError::device_lost);
    }

    // A hit is a map lookup plus a list splice: no rasterizer call, no device call, no allocation.
    const auto found = impl_->index.find(key);
    if (found != impl_->index.end()) {
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return CachedPathTexture{found->second->texture.get(), found->second->extent};
    }
    ++impl_->stats.misses;

    if (key.width == 0U || key.height == 0U || key.scale_milli == 0U) {
        return std::unexpected(PathTextureError::invalid_request);
    }
    const std::uint32_t extent_width = ceil_scale(key.width, key.scale_milli);
    const std::uint32_t extent_height = ceil_scale(key.height, key.scale_milli);
    if (extent_width == 0U || extent_height == 0U || extent_width > kMaxRasterExtent ||
        extent_height > kMaxRasterExtent) {
        return std::unexpected(PathTextureError::invalid_request);
    }

    const std::uint64_t entry_bytes =
        static_cast<std::uint64_t>(extent_width) * extent_height * kBytesPerPixel;
    if (entry_bytes > impl_->config.max_bytes) {
        return std::unexpected(PathTextureError::entry_too_large);
    }

    return impl_->rasterize_and_upload(key, shapes, rhi::Extent2D{extent_width, extent_height});
}

void PathTextureCache::invalidate_all() {
    impl_->clear_entries();
    ++impl_->stats.invalidations;
}

void PathTextureCache::notify_device_lost() {
    impl_->clear_entries();
    impl_->device_lost = true;
    ++impl_->stats.invalidations;
}

std::uint64_t PathTextureCache::generation() const {
    return impl_->generation;
}

std::uint32_t PathTextureCache::entry_count() const {
    return static_cast<std::uint32_t>(impl_->entries.size());
}

std::uint64_t PathTextureCache::byte_count() const {
    return impl_->bytes;
}

PathTextureCache::Stats PathTextureCache::stats() const {
    Stats stats = impl_->stats;
    stats.entries = static_cast<std::uint32_t>(impl_->entries.size());
    stats.bytes = impl_->bytes;
    return stats;
}

} // namespace tessera::rendering::path
