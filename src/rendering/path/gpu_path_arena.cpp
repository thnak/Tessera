// SPDX-License-Identifier: MIT

#include "gpu_path_arena.hpp"

#include <algorithm>
#include <limits>

namespace tessera::rendering::path {
namespace {

constexpr std::size_t kMinBytes = 4096;
constexpr std::uint64_t kIndexLimit = std::numeric_limits<std::uint32_t>::max();

// Shader bindings of the resident buffers: segments 0, strokes 4, dashes (every other binding, 5).
constexpr std::size_t kSegsBinding = 0;
constexpr std::size_t kStrokesBinding = 4;

[[nodiscard]] std::size_t kind_of(std::size_t binding) {
    if (binding == kSegsBinding) {
        return 0U;
    }
    return binding == kStrokesBinding ? 1U : 2U;
}

} // namespace

GpuPathArena::GpuPathArena(rhi::Device& device, std::uint64_t bytes)
    : device_(device) {
    const std::array<std::uint64_t, kKinds> share{bytes / 2U, bytes / 4U, bytes / 4U};
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        capacity_words_[kind] = std::max<std::uint64_t>(share[kind], kMinBytes) / 4U;
        stats_.capacity_bytes += capacity_words_[kind] * 4U;
    }
}

std::optional<GpuPathArena::Slice>
GpuPathArena::acquire(const std::shared_ptr<const EncodedGpuScene>& scene) {
    for (const Entry& entry : entries_) {
        if (entry.scene == scene) {
            ++stats_.hits;
            return entry.slice;
        }
    }
    const std::array<const std::vector<std::uint32_t>*, kKinds> data{&scene->segs, &scene->strokes,
                                                                     &scene->dashes};
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        if (used_words_[kind] + data[kind]->size() > capacity_words_[kind] ||
            used_words_[kind] + data[kind]->size() > kIndexLimit) {
            return std::nullopt;
        }
    }
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        if (!buffers_[kind]) {
            buffers_[kind] = device_.create_buffer(
                {static_cast<std::size_t>(capacity_words_[kind]) * 4U, rhi::BufferUsage::Storage});
            if (!buffers_[kind]) {
                return std::nullopt;
            }
        }
    }
    Slice slice;
    std::array<std::uint32_t*, kKinds> at{&slice.segs, &slice.strokes, &slice.dashes};
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        *at[kind] = static_cast<std::uint32_t>(used_words_[kind]);
        if (!data[kind]->empty()) {
            const std::size_t bytes = data[kind]->size() * sizeof(std::uint32_t);
            buffers_[kind]->write(data[kind]->data(), bytes,
                                  static_cast<std::size_t>(used_words_[kind]) * 4U);
            stats_.uploaded_bytes += bytes;
        }
        used_words_[kind] += data[kind]->size();
    }
    entries_.push_back({scene, slice});
    ++stats_.uploads;
    return slice;
}

void GpuPathArena::reset() {
    entries_.clear();
    used_words_ = {};
    ++stats_.resets;
}

rhi::Buffer& GpuPathArena::buffer(std::size_t binding) const {
    return *buffers_[kind_of(binding)];
}

GpuPathArena::Stats GpuPathArena::stats() const {
    Stats stats = stats_;
    stats.documents = static_cast<std::uint32_t>(entries_.size());
    return stats;
}

} // namespace tessera::rendering::path
