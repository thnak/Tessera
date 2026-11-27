// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tessera::rendering::path {

// A vector document encoded once for the GPU rasterizer (ADR-071, issue #685), independent of the
// size it is drawn at. The geometry is flattened and encoded in the document's own units, and a
// raster at scale s applies the scale as the per-path affine the shader already reads, so changing
// the size re-encodes nothing: it writes 64 bytes of path records per path.
//
// Two things in the encoding depend on the largest scale the document will be drawn at, so a
// document carries the bound it was built for: the flatten tolerance (a chord error of 0.05 px in
// the device at `max_scale` is 0.05 / max_scale in document units) and the round-join arc count (a
// rim scaled up shows its corners). Drawing at a larger scale needs a document built for it
// (`GpuPathDocumentCache` keeps one per power-of-two bucket).
struct GpuPathDocument {
    EncodedGpuScene encoded; // document space; `paths` is empty (records are per raster)
    float max_scale = 1.0f;

    [[nodiscard]] std::size_t byte_size() const noexcept;
};

// The smallest power of two >= `scale`, at least 1 and at most kMaxDocumentScale; 0 for a scale
// that is not finite, not positive or above the maximum (the caller keeps the CPU route).
inline constexpr float kMaxDocumentScale = 32.0f;
[[nodiscard]] float document_scale_bucket(float scale) noexcept;

// Encodes `shapes` (document units) for rasters up to `max_scale`.
[[nodiscard]] std::shared_ptr<const GpuPathDocument>
make_gpu_document(std::span<const PathShape> shapes, float max_scale);

// The affine a raster at `scale` applies to every path of a document: a uniform scale. (A document
// placed with a translation or rotation would compose it here; today a path texture is the document
// at the origin.)
[[nodiscard]] PathAffine document_view(float scale) noexcept;

// Documents by identity, bounded by bytes, least recently used out first. Not thread-safe (the path
// cache that owns it is single-threaded). The identity is the texture key with the size fields
// removed: the same geometry at any size is one document.
class GpuPathDocumentCache {
public:
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0; // encodes
        std::uint64_t evictions = 0;
        std::uint64_t bytes = 0;
        std::uint32_t documents = 0;
    };

    explicit GpuPathDocumentCache(std::uint64_t max_bytes = 8ULL * 1024ULL * 1024ULL);

    // The document for `key`'s geometry built for the bucket of `scale`, encoding `shapes` on a
    // miss. Null when `scale` has no bucket. A hit returns the shared document and encodes nothing.
    [[nodiscard]] std::shared_ptr<const GpuPathDocument>
    acquire(const PathTextureKey& key, std::span<const PathShape> shapes, float scale);

    [[nodiscard]] Stats stats() const;

private:
    struct Entry {
        PathTextureKey key;
        float bucket = 1.0f;
        std::shared_ptr<const GpuPathDocument> document;
        std::uint64_t last_used = 0;
    };
    std::uint64_t max_bytes_;
    std::uint64_t tick_ = 0;
    std::vector<Entry> entries_;
    Stats stats_;
};

} // namespace tessera::rendering::path
