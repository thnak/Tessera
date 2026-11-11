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
    struct Key {
        std::uint64_t hash = 0;
        std::uint32_t width = 0; // the decoded size, 0 x 0 for source size
        std::uint32_t height = 0;
        [[nodiscard]] bool operator==(const Key&) const noexcept = default;
    };
    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept {
            std::uint64_t h = key.hash;
            h ^= (static_cast<std::uint64_t>(key.width) << 32 | key.height) * 0x9E3779B97F4A7C15ULL;
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
    };

    struct Entry {
        Key key;
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
            index.erase(entries.back().key);
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
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    std::uint64_t total_bytes = 0;
    Stats stats;
};

ImageDecodeCache::ImageDecodeCache(ImageDecodeCacheConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ImageDecodeCache::~ImageDecodeCache() = default;

std::expected<std::shared_ptr<const DecodedImage>, ImageError>
ImageDecodeCache::acquire(std::span<const std::uint8_t> bytes, ImageLimits limits,
                          DecodeTarget target) {
    const std::uint64_t hash = content_hash(bytes);
    // The key is the target as asked; a target that turns out not to shrink the image decodes at
    // source size and is stored under the original key below.
    Impl::Key key{hash, target.original() ? 0U : target.width,
                  target.original() ? 0U : target.height};
    auto found = impl_->index.find(key);
    if (found == impl_->index.end() && key.width != 0) {
        // A source-size decode already cached serves any target that would not shrink it.
        const auto original = impl_->index.find(Impl::Key{hash, 0, 0});
        if (original != impl_->index.end()) {
            const DecodedImage& full = *original->second->image;
            if (fit_decode_size(full.source_width, full.source_height, target).width ==
                    full.width &&
                fit_decode_size(full.source_width, full.source_height, target).height ==
                    full.height) {
                found = original;
            }
        }
    }
    if (found != impl_->index.end()) {
        // The entry may have been decoded under looser limits; the caller's limits still apply, so
        // a hit is judged exactly as a fresh decode of the same bytes would be.
        const DecodedImage& cached = *found->second->image;
        if (bytes.empty() || limits.max_input_bytes == 0U || limits.max_width == 0U ||
            limits.max_height == 0U || limits.max_pixels == 0U) {
            return std::unexpected(ImageError::invalid_argument);
        }
        if (bytes.size() > limits.max_input_bytes) {
            return std::unexpected(ImageError::input_too_large);
        }
        if (cached.source_width > limits.max_width || cached.source_height > limits.max_height ||
            static_cast<std::uint64_t>(cached.source_width) * cached.source_height >
                limits.max_pixels) {
            return std::unexpected(ImageError::limit_exceeded);
        }
        // A hit moves the entry to the front (deterministic LRU order).
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return found->second->image;
    }

    ++impl_->stats.misses;
    auto decoded = decode_image(bytes, limits, target);
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
    // A decode that did not shrink is the source-size entry whatever target asked for it.
    if (shared->width == shared->source_width && shared->height == shared->source_height) {
        key = Impl::Key{hash, 0, 0};
    }
    if (const auto stale = impl_->index.find(key); stale != impl_->index.end()) {
        impl_->total_bytes -= stale->second->bytes;
        impl_->entries.erase(stale->second);
        impl_->index.erase(stale);
    }
    impl_->entries.push_front(Impl::Entry{key, shared, bytes_used});
    impl_->index[key] = impl_->entries.begin();
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
