// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_scene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tessera::rendering::path {

std::size_t GpuPathDocument::byte_size() const noexcept {
    const std::size_t words = encoded.segs.size() + encoded.strokes.size() + encoded.dashes.size();
    return words * sizeof(std::uint32_t) + encoded.dash_base.size() * sizeof(float) +
           encoded.local_boxes.size() * sizeof(std::array<float, 4>) +
           (encoded.rules.size() + encoded.colors.size()) * sizeof(std::uint32_t) + sizeof(*this);
}

float document_scale_bucket(float scale) noexcept {
    if (!std::isfinite(scale) || !(scale > 0.0f) || scale > kMaxDocumentScale) {
        return 0.0f;
    }
    float bucket = 1.0f;
    while (bucket < scale) {
        bucket *= 2.0f;
    }
    return bucket;
}

std::shared_ptr<const GpuPathDocument> make_gpu_document(std::span<const PathShape> shapes,
                                                         float max_scale) {
    auto document = std::make_shared<GpuPathDocument>();
    document->max_scale = std::max(max_scale, 1.0f);
    // 0.05 px of chord error in the device at the largest scale, in document units.
    const GpuPathScene scene = gpu_scene_from_shapes(shapes, 0.05f / document->max_scale);
    // The size arguments only clamp the per-path device boxes, which a document does not keep.
    document->encoded = encode_gpu_scene(scene, 0, 0, document->max_scale);
    document->encoded.paths.clear();
    document->encoded.paths.shrink_to_fit();
    return document;
}

PathAffine document_view(float scale) noexcept {
    PathAffine view;
    view.a = scale;
    view.d = scale;
    return view;
}

GpuPathDocumentCache::GpuPathDocumentCache(std::uint64_t max_bytes)
    : max_bytes_(max_bytes) {}

std::shared_ptr<const GpuPathDocument>
GpuPathDocumentCache::acquire(const PathTextureKey& key, std::span<const PathShape> shapes,
                              float scale) {
    const float bucket = document_scale_bucket(scale);
    if (bucket == 0.0f) {
        return nullptr;
    }
    // The same geometry at any size is one document: the size fields are not identity.
    PathTextureKey identity = key;
    identity.width = 0;
    identity.height = 0;
    identity.scale_milli = 1000;
    ++tick_;
    for (Entry& entry : entries_) {
        if (entry.bucket == bucket && entry.key == identity) {
            entry.last_used = tick_;
            ++stats_.hits;
            return entry.document;
        }
    }
    ++stats_.misses;
    auto document = make_gpu_document(shapes, bucket);
    const std::uint64_t cost = document->byte_size();
    // Make room by dropping the least recently used documents. A document the rasterizer still
    // holds in a queued job stays alive through its shared pointer; only the cache forgets it.
    while (!entries_.empty() && stats_.bytes + cost > max_bytes_) {
        const auto oldest =
            std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
                return a.last_used < b.last_used;
            });
        stats_.bytes -= oldest->document->byte_size();
        entries_.erase(oldest);
        ++stats_.evictions;
    }
    stats_.bytes += cost;
    entries_.push_back({identity, bucket, document, tick_});
    stats_.documents = static_cast<std::uint32_t>(entries_.size());
    return document;
}

GpuPathDocumentCache::Stats GpuPathDocumentCache::stats() const {
    Stats stats = stats_;
    stats.documents = static_cast<std::uint32_t>(entries_.size());
    return stats;
}

} // namespace tessera::rendering::path
