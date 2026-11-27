// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/gpu_path_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tessera::rendering::path {

// The CPU half of the GPU path rasterizer (ADR-071): packs a GpuPathScene into the word buffers the
// four compute passes read (shaders/path_raster.comp), and plans the work buffer and push block.
// Word layouts are the shader's contract; the manifest entry `path_raster` lists the bindings.
//
// Fill edges, stroke records and dash records are encoded once; the per-path records (transform,
// device box) are rewritten per frame with `write_path_records`, and a dash phase with
// `write_dash_offsets`, so an animated frame uploads 64 + 4 bytes per path and nothing else.

inline constexpr std::size_t kGpuSegWords = 8; // x0 y0 x1 y1 (f32), path, 3 spare
inline constexpr std::size_t kGpuPathWords =
    16; // affine (6 f32), rule, rgba, device box (4), identity flag, 3 spare
inline constexpr std::size_t kGpuStrokeWords = 16; // see the Strokes buffer comment in the shader
inline constexpr std::size_t kGpuDashWords = 12;   // count, offset, period, -, 8 lengths

struct EncodedGpuScene {
    std::vector<std::uint32_t> segs;    // fill edges, kGpuSegWords each
    std::vector<std::uint32_t> strokes; // kGpuStrokeWords per stroke record (a segment or a chunk)
    std::vector<std::uint32_t> dashes;  // kGpuDashWords per path
    std::vector<float> dash_base;       // each path's own offset, before any animation
    std::vector<std::uint32_t> paths;   // kGpuPathWords per path (see write_path_records)
    std::vector<std::array<float, 4>> local_boxes; // min x, min y, max x, max y in path space
    std::vector<std::uint32_t> rules;
    std::vector<std::uint32_t> colors;
    std::uint32_t fill_count = 0;   // fill edges (the segs buffer)
    std::uint32_t stroke_count = 0; // stroke records
    std::uint32_t stroke_edges = 0; // expanded edge slots over every record, pieces included
    std::uint32_t seg_count = 0;    // fill_count + stroke_edges: the bin pass's thread count
    std::uint32_t arc_edges = 0;    // edges per join wedge or cap
    std::uint32_t path_count = 0;
    std::uint64_t row_nodes = 0; // upper bound on bin nodes at scale 1: segment rows summed
};

// `max_scale` is the largest uniform scale a path transform can reach (1 for none): it widens the
// round-join arc count so a scaled-up rim keeps its chord error, and the row-node bound.
[[nodiscard]] EncodedGpuScene encode_gpu_scene(const GpuPathScene& scene, std::uint32_t width,
                                               std::uint32_t height, float max_scale = 1.0f);

// The per-path records for one raster of `encoded` as a new word vector (kGpuPathWords per path),
// without touching `encoded`: the same affine and clamped device box as `write_path_records`. A
// shared, immutable encoded scene (GpuPathDocument) is rasterized at many scales by giving each
// raster its own records.
[[nodiscard]] std::vector<std::uint32_t> make_path_records(const EncodedGpuScene& encoded,
                                                           std::span<const PathAffine> matrices,
                                                           std::uint32_t width,
                                                           std::uint32_t height);

// Rewrites the per-path records for one frame: the affine (an empty span is identity for every
// path, otherwise one per path), and the device-space box bounding the transformed local box,
// clamped to the target, which the fine pass uses to skip a pixel outside it.
void write_path_records(EncodedGpuScene& encoded, std::span<const PathAffine> matrices,
                        std::uint32_t width, std::uint32_t height);

// Sets each dashed path's phase to its encoded offset plus `phase[p]` (solid paths are untouched).
// `phase` holds one entry per path.
void write_dash_offsets(EncodedGpuScene& encoded, std::span<const float> phase);

// The sizes of one raster, mirrored from the shader's `Job`: the first 12 words of a job record.
struct PathRasterPush {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t path_count = 0;
    std::uint32_t seg_count = 0;
    std::uint32_t node_capacity = 0; // bin nodes the work buffer holds
    std::uint32_t nodes_base = 0;    // first word of the node pool inside the work buffer
    std::uint32_t fill_count = 0;
    std::uint32_t exp_base = 0; // first word of the GPU-expanded stroke edges inside work
    std::uint32_t stroke_count = 0;
    std::uint32_t arc_edges = 0;
    std::uint32_t out_stride = 0; // words between output rows (the copy's pitch / 4)
    // First word of the device-space segment records inside work (4 floats per segment id: the
    // endpoints after the path transform, written by the bin pass and read by the fine pass).
    std::uint32_t dev_base = 0;
};
static_assert(sizeof(PathRasterPush) == 48);

// One record of the `jobs` buffer (binding 6): a raster's sizes plus where its slices start inside
// the shared segs / paths / work / output / strokes / dashes buffers, in 32-bit words. A batch of N
// rasters is N records and one dispatch of each pass; a single raster is one record with every
// base 0 (`single_raster_job`).
inline constexpr std::size_t kPathRasterJobWords = 20;
struct PathRasterJob {
    PathRasterPush push;
    std::uint32_t seg_base = 0;
    std::uint32_t path_base = 0;
    std::uint32_t work_base = 0;
    std::uint32_t out_base = 0;
    std::uint32_t stroke_base = 0;
    std::uint32_t dash_base = 0;
    std::uint32_t spare[2] = {0, 0};
};
static_assert(sizeof(PathRasterJob) == kPathRasterJobWords * sizeof(std::uint32_t));

struct PathRasterLayout {
    PathRasterPush push;
    std::size_t work_words = 0;   // size of the gpu_read_write work buffer, in 32-bit words
    std::size_t output_words = 0; // out_stride * height
};

// Sizes the work buffer and fills the push block for a `width` x `height` target whose rows are
// `out_stride_words` apart (0 = tightly packed; pass `aligned_texture_copy_pitch(width) / 4` when
// the output feeds `copy_buffer_to_texture`). `max_scale` is the same bound given to the encoder.
// Returns nothing when a size does not fit the shader's 32-bit indices.
[[nodiscard]] PathRasterJob single_raster_job(const PathRasterLayout& layout);

[[nodiscard]] std::optional<PathRasterLayout>
plan_path_raster(const EncodedGpuScene& encoded, std::uint32_t width, std::uint32_t height,
                 std::uint32_t out_stride_words = 0, float max_scale = 1.0f);

} // namespace tessera::rendering::path
