// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace tessera::rendering::path {

// Device-resident geometry for the GPU path rasterizer (ADR-071, issue #685): the fill edges,
// stroke records and dash records of a document, uploaded once and addressed by word offsets
// from every later raster of that document, so a steady animated frame uploads only its job table
// and per-path records. Three bump-allocated buffers (bindings 0, 4 and 5 of the shader).
//
// There is no per-document free: when the arena is full the owner resets it as a whole, which is
// only legal while no queued or in-flight raster reads it (`GpuPathRasterizer` checks). The arena
// keeps a shared pointer to each resident scene, so the key (the scene's address) cannot be reused
// by another scene while its entry lives. Not thread-safe.
class GpuPathArena {
public:
    struct Slice {
        std::uint32_t segs = 0; // word offsets into the three buffers
        std::uint32_t strokes = 0;
        std::uint32_t dashes = 0;
    };

    struct Stats {
        std::uint64_t uploads = 0; // documents uploaded
        std::uint64_t hits = 0;    // lookups served by a resident document
        std::uint64_t resets = 0;
        std::uint64_t uploaded_bytes = 0;
        std::uint32_t documents = 0;
        std::uint64_t capacity_bytes = 0;
    };

    // `bytes` is split between the buffers 1/2 fill edges, 1/4 strokes, 1/4 dashes (each at least
    // 4 KiB). The buffers are created on first use.
    GpuPathArena(rhi::Device& device, std::uint64_t bytes);

    // The resident slice of `scene`, uploading it first when it is not there. Nothing when it does
    // not fit (after a reset the caller may retry). `scene` must be a document's encoded scene.
    [[nodiscard]] std::optional<Slice> acquire(const std::shared_ptr<const EncodedGpuScene>& scene);

    // Forgets every resident document. The caller guarantees no raster reads the buffers.
    void reset();

    // The buffer for shader binding 0 (segs), 4 (strokes) or 5 (dashes). Valid after acquire().
    [[nodiscard]] rhi::Buffer& buffer(std::size_t binding) const;

    [[nodiscard]] Stats stats() const;

private:
    struct Entry {
        std::shared_ptr<const EncodedGpuScene> scene;
        Slice slice;
    };
    static constexpr std::size_t kKinds = 3; // segs, strokes, dashes

    rhi::Device& device_;
    std::array<std::uint64_t, kKinds> capacity_words_{};
    std::array<std::uint64_t, kKinds> used_words_{};
    std::array<std::unique_ptr<rhi::Buffer>, kKinds> buffers_;
    std::vector<Entry> entries_;
    Stats stats_;
};

} // namespace tessera::rendering::path
