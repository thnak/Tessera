// SPDX-License-Identifier: MIT

#include "tessera/rendering/image/image_decode_cache.hpp"

#include <algorithm>
#include <list>
#include <utility>

namespace tessera::rendering::image {
namespace {

// FNV-1a content hash, matching the identity rule used by the vector document/texture keys: the
// hash only selects a bucket, and colliding content is an accepted (documented) risk rather than a
// correctness break, because an embedded bundle cannot contain two assets with the same bytes.
[[nodiscard]] std::uint64_t content_hash(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

std::uint64_t decoded_bytes(const DecodedImage& image) noexcept {
    return static_cast<std::uint64_t>(image.rgba.size());
}

} // namespace

struct ImageDecodeCache::Impl {
    struct Entry {
        std::uint64_t hash = 0;
        std::shared_ptr<const DecodedImage> image;
        std::uint64_t bytes = 0;
    };

    explicit Impl(ImageDecodeCacheConfig cache_config)
        : config(std::move(cache_config)) {
        config.max_entries = std::max(2U, config.max_entries);
        config.max_bytes = std::max<std::uint64_t>(4ULL, config.max_bytes);
    }

    void evict_until_fits(std::uint64_t incoming_bytes) {
        while (entries.size() >= config.max_entries ||
               total_bytes + incoming_bytes > config.max_bytes) {
            if (entries.empty()) {
                break;
            }
            total_bytes -= entries.back().bytes;
            index.erase(entries.back().hash);
            entries.pop_back();
            ++stats.evictions;
        }
    }

    [[nodiscard]] Stats snapshot() const {
        Stats result = stats;
        result.entries = static_cast<std::uint32_t>(entries.size());
        result.bytes = total_bytes;
        return result;
    }

    ImageDecodeCacheConfig config;
    std::list<Entry> entries;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> index;
    std::uint64_t total_bytes = 0;
    Stats stats;
};

ImageDecodeCache::ImageDecodeCache(ImageDecodeCacheConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ImageDecodeCache::~ImageDecodeCache() = default;

std::expected<std::shared_ptr<const DecodedImage>, ImageError>
ImageDecodeCache::acquire(std::span<const std::uint8_t> bytes, ImageLimits limits) {
    const std::uint64_t hash = content_hash(bytes);
    if (const auto found = impl_->index.find(hash); found != impl_->index.end()) {
        // A hit moves the entry to the front (deterministic LRU order).
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return found->second->image;
    }

    ++impl_->stats.misses;
    auto decoded = decode_image(bytes, limits);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    ++impl_->stats.decodes;
    decoded->content_hash = hash;
    auto shared = std::make_shared<const DecodedImage>(std::move(*decoded));
    const std::uint64_t bytes_used = decoded_bytes(*shared);
    if (bytes_used > impl_->config.max_bytes) {
        // Not cacheable: return it without retaining it, so a single huge image cannot flush the
        // whole cache on every request.
        return shared;
    }
    impl_->evict_until_fits(bytes_used);
    impl_->entries.push_front(Impl::Entry{hash, shared, bytes_used});
    impl_->index[hash] = impl_->entries.begin();
    impl_->total_bytes += bytes_used;
    return shared;
}

void ImageDecodeCache::clear() {
    impl_->entries.clear();
    impl_->index.clear();
    impl_->total_bytes = 0;
}

ImageDecodeCache::Stats ImageDecodeCache::stats() const {
    return impl_->snapshot();
}

} // namespace tessera::rendering::image
