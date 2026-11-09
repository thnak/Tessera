// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/svg_document_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>

namespace tessera::rendering::path {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    return (hash ^ value) * kFnvPrime;
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * kFnvPrime;
    }
    return hash;
}

struct SvgDocumentKeyHash {
    [[nodiscard]] std::size_t operator()(const SvgDocumentKey& key) const noexcept {
        std::uint64_t hash = mix(key.content_hash, static_cast<std::uint64_t>(key.byte_size));
        hash = mix(hash, static_cast<std::uint64_t>(key.limits.max_input_bytes));
        hash = mix(hash, key.limits.max_shapes);
        hash = mix(hash, key.limits.max_depth);
        hash = mix(hash, key.limits.max_segments);
        hash = mix(hash, key.limits.max_extent);
        return static_cast<std::size_t>(hash);
    }
};

[[nodiscard]] std::uint32_t count_segments(const SvgDocument& document) noexcept {
    std::uint64_t total = 0;
    for (const PathShape& shape : document.shapes) {
        total += shape.path.segments.size();
    }
    return total > static_cast<std::uint64_t>(UINT32_MAX) ? UINT32_MAX
                                                          : static_cast<std::uint32_t>(total);
}

} // namespace

SvgDocumentKey make_svg_document_key(std::span<const std::uint8_t> bytes,
                                     const SvgLimits& limits) noexcept {
    return SvgDocumentKey{fnv1a64(bytes), bytes.size(), limits};
}

struct SvgDocumentCache::Impl {
    struct Entry {
        SvgDocumentKey key;
        SvgDocument document;
        std::uint32_t shapes = 0;
        std::uint32_t segments = 0;
    };

    Impl(SvgDocumentCacheConfig cache_config, SvgLimits svg_limits)
        : config(cache_config),
          decode_limits(svg_limits) {
        config.max_entries = std::max(1U, config.max_entries);
        config.max_shapes = std::max(1U, config.max_shapes);
        config.max_segments = std::max(1U, config.max_segments);
    }

    void clear_entries() {
        entries.clear();
        index.clear();
        shapes_total = 0;
        segments_total = 0;
    }

    void evict_until_fits(std::uint32_t incoming_shapes, std::uint32_t incoming_segments) {
        while (!entries.empty()) {
            const bool over_entries = entries.size() >= config.max_entries;
            const bool over_shapes =
                static_cast<std::uint64_t>(shapes_total) + incoming_shapes > config.max_shapes;
            const bool over_segments =
                static_cast<std::uint64_t>(segments_total) + incoming_segments >
                config.max_segments;
            if (!over_entries && !over_shapes && !over_segments) {
                break;
            }
            const Entry& victim = entries.back();
            shapes_total -= victim.shapes;
            segments_total -= victim.segments;
            index.erase(victim.key);
            entries.pop_back();
            ++stats.evictions;
        }
    }

    SvgDocumentCacheConfig config;
    SvgLimits decode_limits;
    std::list<Entry> entries; // front = most recently used, eviction takes the back
    std::unordered_map<SvgDocumentKey, std::list<Entry>::iterator, SvgDocumentKeyHash> index;
    std::uint32_t shapes_total = 0;
    std::uint32_t segments_total = 0;
    Stats stats;
};

SvgDocumentCache::SvgDocumentCache(SvgDocumentCacheConfig config, SvgLimits decode_limits)
    : impl_(std::make_unique<Impl>(config, decode_limits)) {}

SvgDocumentCache::~SvgDocumentCache() = default;

std::expected<const SvgDocument*, SvgError>
SvgDocumentCache::acquire(const SvgDocumentKey& key, std::span<const std::uint8_t> bytes) {
    // A hit is one hash lookup plus a list splice: no decode, no allocation.
    const auto found = impl_->index.find(key);
    if (found != impl_->index.end()) {
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return &found->second->document;
    }
    ++impl_->stats.misses;
    ++impl_->stats.decodes;

    auto decoded = load_svg(bytes, key.limits);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error()); // failures are never cached
    }
    const auto shapes = static_cast<std::uint32_t>(
        std::min<std::size_t>(decoded->shapes.size(), static_cast<std::size_t>(UINT32_MAX)));
    const std::uint32_t segments = count_segments(*decoded);
    if (shapes > impl_->config.max_shapes || segments > impl_->config.max_segments) {
        return std::unexpected(SvgError::limit_exceeded); // one document above the total budget
    }

    impl_->evict_until_fits(shapes, segments);
    impl_->entries.emplace_front(Impl::Entry{key, std::move(*decoded), shapes, segments});
    impl_->index.emplace(key, impl_->entries.begin());
    impl_->shapes_total += shapes;
    impl_->segments_total += segments;
    return &impl_->entries.front().document;
}

std::expected<const SvgDocument*, SvgError>
SvgDocumentCache::acquire(std::span<const std::uint8_t> bytes) {
    const SvgDocumentKey key = make_svg_document_key(bytes, impl_->decode_limits);
    return acquire(key, bytes);
}

std::expected<const SvgDocument*, SvgError> SvgDocumentCache::acquire(std::span<const char> bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return acquire(std::span<const std::uint8_t>(data, bytes.size()));
}

std::size_t SvgDocumentCache::invalidate(const SvgDocumentKey& key) {
    const auto found = impl_->index.find(key);
    if (found == impl_->index.end()) {
        return 0;
    }
    impl_->shapes_total -= found->second->shapes;
    impl_->segments_total -= found->second->segments;
    impl_->entries.erase(found->second);
    impl_->index.erase(found);
    ++impl_->stats.invalidations;
    return 1;
}

std::size_t SvgDocumentCache::invalidate_all() {
    const std::size_t dropped = impl_->entries.size();
    impl_->clear_entries();
    ++impl_->stats.invalidations;
    return dropped;
}

std::uint32_t SvgDocumentCache::entry_count() const {
    return static_cast<std::uint32_t>(impl_->entries.size());
}

std::uint32_t SvgDocumentCache::shape_count() const {
    return impl_->shapes_total;
}

std::uint32_t SvgDocumentCache::segment_count() const {
    return impl_->segments_total;
}

const SvgLimits& SvgDocumentCache::decode_limits() const {
    return impl_->decode_limits;
}

SvgDocumentCache::Stats SvgDocumentCache::stats() const {
    Stats stats = impl_->stats;
    stats.entries = static_cast<std::uint32_t>(impl_->entries.size());
    stats.shapes = impl_->shapes_total;
    stats.segments = impl_->segments_total;
    return stats;
}

} // namespace tessera::rendering::path
