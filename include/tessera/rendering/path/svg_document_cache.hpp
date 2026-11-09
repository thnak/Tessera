// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/svg_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace tessera::rendering::path {

// Content identity of a decoded asset. `content_hash` is FNV-1a 64 over the raw SVG bytes (stable
// across runs, processes and working directories); `byte_size` and the decode-limits signature are
// part of the key so equal-looking requests with different decode policies never alias. FNV-1a 64
// is a cache key, not a security boundary: a deliberate 64-bit collision could alias two assets,
// which the byte_size field narrows further (documented asset-cache tradeoff).
struct SvgDocumentKey {
    std::uint64_t content_hash = 0;
    std::size_t byte_size = 0;
    SvgLimits limits;

    [[nodiscard]] bool operator==(const SvgDocumentKey&) const noexcept = default;
};

[[nodiscard]] SvgDocumentKey make_svg_document_key(std::span<const std::uint8_t> bytes,
                                                   const SvgLimits& limits = {}) noexcept;

struct SvgDocumentCacheConfig {
    // Bounded LRU. Both budgets are enforced exactly: entries <= max_entries and the total decoded
    // geometry across entries stays inside max_shapes / max_segments. One decoded document is one
    // entry, so the defaults (64 entries, 4096 shapes, 131072 segments) bound the retained geometry
    // rather than the asset count. A single document above a budget is rejected with
    // SvgError::limit_exceeded and never cached.
    std::uint32_t max_entries = 64;
    std::uint32_t max_shapes = 4096;
    std::uint32_t max_segments = 131072;
};

// Decodes an SVG once and keeps the SvgDocument for later rasterizations. A hit is a hash lookup
// plus a list splice: no decode, no allocation. The returned pointer is stable until that entry is
// evicted or invalidated; re-acquire after those events. Not thread-safe (one instance per thread).
class SvgDocumentCache {
public:
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t decodes = 0; // load_svg attempts, including failed ones
        std::uint64_t evictions = 0;
        std::uint64_t invalidations = 0;
        std::uint32_t entries = 0;
        std::uint32_t shapes = 0;
        std::uint32_t segments = 0;
    };

    explicit SvgDocumentCache(SvgDocumentCacheConfig config = {}, SvgLimits decode_limits = {});
    ~SvgDocumentCache();

    SvgDocumentCache(const SvgDocumentCache&) = delete;
    SvgDocumentCache& operator=(const SvgDocumentCache&) = delete;
    SvgDocumentCache(SvgDocumentCache&&) = delete;
    SvgDocumentCache& operator=(SvgDocumentCache&&) = delete;

    // Cached decode. On a hit `bytes` is ignored (pass empty data when you already hold the key);
    // on a miss the bytes plus `key.limits` are decoded. A failed decode is never cached, so the
    // next acquire retries and increments `decodes` again.
    [[nodiscard]] std::expected<const SvgDocument*, SvgError>
    acquire(const SvgDocumentKey& key, std::span<const std::uint8_t> bytes);

    // Content-keyed convenience overloads using this cache's decode limits.
    [[nodiscard]] std::expected<const SvgDocument*, SvgError>
    acquire(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::expected<const SvgDocument*, SvgError> acquire(std::span<const char> bytes);

    // Drops one entry (1 when it existed) or every entry; returns how many were dropped.
    std::size_t invalidate(const SvgDocumentKey& key);
    std::size_t invalidate_all();

    [[nodiscard]] std::uint32_t entry_count() const;
    [[nodiscard]] std::uint32_t shape_count() const;
    [[nodiscard]] std::uint32_t segment_count() const;
    [[nodiscard]] const SvgLimits& decode_limits() const;
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::rendering::path
