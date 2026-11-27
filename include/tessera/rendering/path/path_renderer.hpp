// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace tessera::rendering::path {

struct GpuPathDocument;

enum class PathRenderError : std::uint8_t {
    unsupported = 0, // the device has no compute or no buffer-to-texture copy
    batch_full,      // the batch is full: flush(), then enqueue again
    too_large,       // a size does not fit the renderer's indices or the dispatch limit
    too_dense,       // the raster has more work than the caller's bound: the CPU is faster
    device_failure,  // the device refused a buffer, pipeline or submission
};

// The seam between path consumers (PathTextureCache, and through it PaintSink vector draws, icons
// and charts) and a renderer that turns vector paths into a texture on the device (ADR-071, issue
// #685). It sits beside PathRasterizer, whose contract is an RGBA8 buffer on the CPU and which
// cannot express "render into this texture, later, without waiting".
//
// The contract is asynchronous and batched: enqueue() queues a raster into a texture the caller
// created, flush() submits the queue in one go without waiting, collect() releases what the device
// finished (never blocks), and a texture is ready to sample by the draw list submitted after the
// flush on the same queue. Not thread-safe. GpuPathRasterizer is the compute implementation; a
// renderer that blends straight into a frame target would be another one.
class PathRenderer {
public:
    virtual ~PathRenderer() = default;

    // True when this renderer can run on its device right now (the route policy asks before
    // choosing the device over the CPU raster).
    [[nodiscard]] virtual bool available() const = 0;

    // Queues `shapes` (already scaled to the target) to be rendered into `target`, a texture of
    // `extent` this device created. `target` must stay alive until the batch is collected. A
    // non-zero `max_row_nodes` refuses a raster whose bin-node bound is larger with `too_dense`.
    [[nodiscard]] virtual std::expected<void, PathRenderError>
    enqueue(std::span<const PathShape> shapes, rhi::Texture& target, rhi::Extent2D extent,
            std::uint64_t max_row_nodes) = 0;

    // Queues a scale-independent `document` at `scale`: the encoded geometry is shared between
    // every size, so only the per-raster records are built.
    [[nodiscard]] virtual std::expected<void, PathRenderError>
    enqueue(std::shared_ptr<const GpuPathDocument> document, float scale, rhi::Texture& target,
            rhi::Extent2D extent, std::uint64_t max_row_nodes) = 0;

    // Drops the queued rasters without submitting them (their targets are going away).
    virtual void discard_pending() = 0;

    // Submits every queued raster without waiting. A no-op without pending work.
    [[nodiscard]] virtual std::expected<void, PathRenderError> flush() = 0;

    // Releases the resources of submitted batches the device has finished. Never blocks.
    virtual void collect() = 0;
};

} // namespace tessera::rendering::path
