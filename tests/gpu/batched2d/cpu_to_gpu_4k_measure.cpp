// SPDX-License-Identifier: MIT
//
// Manual measurement tool (not part of the CTest run): what the CPU-rasterize-then-upload paths
// cost at 4K on a real Vulkan device. It exists to put numbers behind the decision to move vector
// paths (and other CPU-produced pixels) onto the GPU, instead of guessing:
//
//   1. ThorVG CPU raster of a vector scene into a 3840x2160 RGBA8 buffer;
//   2. the RHI upload of that buffer (create_texture and Texture::write), split into the CPU-side
//      cost (the call returns, the copy is queued) and the time until the GPU has the pixels;
//   3. the image path: deterministic nearest resample + upload through ImageTextureCache;
//   4. glyph-atlas page style whole-page writes;
//   5. a plain memcpy of the same bytes, the host-bandwidth floor the staging copy cannot beat.
//
// Every row prints min/median/max over the iterations, then a budget table against 60/120/240 Hz.

#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace path = tessera::rendering::path;
namespace image = tessera::rendering::image;
namespace rhi = tessera::rendering::rhi;

constexpr std::uint32_t k4kWidth = 3840;
constexpr std::uint32_t k4kHeight = 2160;
constexpr int kWarmup = 2;
constexpr int kIterations = 15;

using Clock = std::chrono::steady_clock;

[[nodiscard]] double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Sample {
    double min = 0.0;
    double median = 0.0;
    double max = 0.0;
};

[[nodiscard]] Sample summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values.front(), values[values.size() / 2], values.back()};
}

[[nodiscard]] Sample time_it(const std::function<void()>& body) {
    for (int i = 0; i < kWarmup; ++i) {
        body();
    }
    std::vector<double> values;
    for (int i = 0; i < kIterations; ++i) {
        const auto start = Clock::now();
        body();
        values.push_back(ms_since(start));
    }
    return summarize(std::move(values));
}

void print_row(const std::string& name, const Sample& sample, std::uint64_t bytes) {
    // bytes / ms / 1e6 = GB/s with decimal giga.
    const double gbps =
        sample.median > 0.0 ? static_cast<double>(bytes) / (sample.median * 1.0e6) : 0.0;
    std::cout << std::left << std::setw(46) << name << std::right << std::fixed
              << std::setprecision(3) << " min=" << std::setw(8) << sample.min
              << " med=" << std::setw(8) << sample.median << " max=" << std::setw(8) << sample.max
              << " ms  " << std::setprecision(2) << std::setw(6) << gbps << " GB/s\n";
}

void print_budget(const std::string& name, double median_ms) {
    std::cout << "  " << std::left << std::setw(44) << name << std::right << std::fixed
              << std::setprecision(1) << std::setw(7) << median_ms << " ms  = " << std::setw(5)
              << (median_ms / (1000.0 / 60.0)) * 100.0 << "% of 60Hz, " << std::setw(5)
              << (median_ms / (1000.0 / 120.0)) * 100.0 << "% of 120Hz, " << std::setw(5)
              << (median_ms / (1000.0 / 240.0)) * 100.0 << "% of 240Hz\n";
}

// A closed rectangle path.
[[nodiscard]] path::Path rect_path(float x0, float y0, float x1, float y1) {
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {x0, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x0, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

// A closed four-cubic blob (circle-like) centered at (cx, cy) with radius r.
[[nodiscard]] path::Path blob_path(float cx, float cy, float r) {
    constexpr float k = 0.5522847498f;
    const float c = r * k;
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {cx + r, cy}, {}, {}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx + r, cy + c}, {cx + c, cy + r}, {cx, cy + r}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx - c, cy + r}, {cx - r, cy + c}, {cx - r, cy}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx - r, cy - c}, {cx - c, cy - r}, {cx, cy - r}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx + c, cy - r}, {cx + r, cy - c}, {cx + r, cy}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

[[nodiscard]] path::PathShape filled(path::Path geometry, path::Rgba8 color) {
    path::PathShape shape;
    shape.path = std::move(geometry);
    shape.paint.fill = color;
    return shape;
}

[[nodiscard]] path::PathShape stroked(path::Path geometry, path::Rgba8 color, float width) {
    path::PathShape shape;
    shape.path = std::move(geometry);
    path::StrokeStyle stroke;
    stroke.color = color;
    stroke.width = width;
    stroke.cap = path::LineCap::round;
    stroke.join = path::LineJoin::round;
    shape.paint.stroke = stroke;
    return shape;
}

// "Sparse": icon/chart-like content, 15 small shapes. Raster cost should be dominated by clearing
// and compositing a small share of the 8.3M pixels.
[[nodiscard]] std::vector<path::PathShape> sparse_scene() {
    std::vector<path::PathShape> shapes;
    for (int i = 0; i < 15; ++i) {
        const float cx = 300.0f + static_cast<float>(i % 5) * 700.0f;
        const float cy = 400.0f + static_cast<float>(i / 5) * 650.0f;
        shapes.push_back(filled(blob_path(cx, cy, 120.0f), {40, 160, 220, 255}));
        shapes.push_back(stroked(blob_path(cx, cy, 150.0f), {220, 80, 60, 255}, 8.0f));
    }
    return shapes;
}

// "Dense": big-area content, the case the owner worries about. Large fills cover most of the
// frame, so raster cost scales with area rather than with shape count.
[[nodiscard]] std::vector<path::PathShape> dense_scene() {
    std::vector<path::PathShape> shapes;
    shapes.push_back(filled(rect_path(0, 0, k4kWidth, k4kHeight), {20, 24, 32, 255}));
    for (int i = 0; i < 6; ++i) {
        const float f = static_cast<float>(i);
        shapes.push_back(filled(blob_path(1920.0f + f * 40.0f, 1080.0f, 1000.0f - f * 120.0f),
                                {static_cast<std::uint8_t>(40 + i * 30), 120, 200, 200}));
        shapes.push_back(stroked(blob_path(1920.0f, 1080.0f + f * 30.0f, 1050.0f - f * 100.0f),
                                 {240, 200, 60, 220}, 14.0f));
    }
    return shapes;
}

struct Scene {
    const char* name;
    std::vector<path::PathShape> shapes;
};

// Per-call latency distribution: the spike hypothesis is about the tail, so median alone hides it.
struct Tail {
    double p50 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    std::vector<std::size_t> spike_indices; // calls slower than 3x the median
};

[[nodiscard]] Tail tail_of(const std::vector<double>& values) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    Tail tail;
    tail.p50 = sorted[sorted.size() / 2];
    tail.p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    tail.max = sorted.back();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] > 3.0 * tail.p50) {
            tail.spike_indices.push_back(i);
        }
    }
    return tail;
}

void print_tail(const std::string& name, const Tail& tail, std::size_t calls) {
    std::cout << std::left << std::setw(46) << name << std::right << std::fixed
              << std::setprecision(3) << " p50=" << std::setw(8) << tail.p50
              << " p99=" << std::setw(8) << tail.p99 << " max=" << std::setw(8) << tail.max
              << " ms  spikes(>3x p50)=" << tail.spike_indices.size() << "/" << calls;
    if (!tail.spike_indices.empty()) {
        std::cout << " at call#";
        for (std::size_t i = 0; i < std::min<std::size_t>(tail.spike_indices.size(), 8); ++i) {
            std::cout << ' ' << tail.spike_indices[i];
        }
    }
    std::cout << "\n";
}

// H1: are the CPU-side upload spikes the per-call staging allocation? Time the allocation, the
// write into a reused buffer and the whole Texture::write separately, per call.
void measure_staging_hypothesis(rhi::Device& device, const std::vector<std::uint8_t>& frame) {
    constexpr std::size_t kCalls = 200;
    std::cout << "\n[H1. Where do the upload spikes come from? " << kCalls << " calls each]\n";

    std::vector<double> alloc;
    for (std::size_t i = 0; i < kCalls; ++i) {
        const auto start = Clock::now();
        auto buffer = device.create_buffer({frame.size(), rhi::BufferUsage::Staging});
        alloc.push_back(ms_since(start));
    }
    print_tail("create_buffer(Staging, 31.6 MiB) only", tail_of(alloc), kCalls);

    auto reused = device.create_buffer({frame.size(), rhi::BufferUsage::Staging});
    std::vector<double> reuse;
    for (std::size_t i = 0; i < kCalls; ++i) {
        const auto start = Clock::now();
        reused->write(frame.data(), frame.size());
        reuse.push_back(ms_since(start));
    }
    print_tail("Buffer::write into ONE reused staging buffer", tail_of(reuse), kCalls);

    rhi::TextureDesc blank;
    blank.extent = {k4kWidth, k4kHeight};
    auto texture = device.create_texture(blank);
    device.wait_idle();
    std::vector<double> whole;
    for (std::size_t i = 0; i < kCalls; ++i) {
        const auto start = Clock::now();
        texture->write(frame.data(), frame.size());
        whole.push_back(ms_since(start));
        if (i % 8 == 7) {
            device.wait_idle(); // untimed: keeps pending uploads bounded like a real frame loop
        }
    }
    device.wait_idle();
    print_tail("Texture::write (alloc + memcpy + submit)", tail_of(whole), kCalls);
}

// H2: the resample loop on its own, same arithmetic as ImageTextureCache's resample_nearest.
[[nodiscard]] std::vector<std::uint8_t> resample_like_cache(const std::vector<std::uint8_t>& src,
                                                            std::uint32_t src_w,
                                                            std::uint32_t src_h, std::uint32_t w,
                                                            std::uint32_t h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4U, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint64_t ny = (2ULL * y + 1ULL) * src_h;
        const std::uint32_t sy =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(ny / (2ULL * h)), src_h - 1U);
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint64_t nx = (2ULL * x + 1ULL) * src_w;
            const std::uint32_t sx =
                std::min<std::uint32_t>(static_cast<std::uint32_t>(nx / (2ULL * w)), src_w - 1U);
            const std::size_t s = (static_cast<std::size_t>(sy) * src_w + sx) * 4U;
            const std::size_t d = (static_cast<std::size_t>(y) * w + x) * 4U;
            for (std::uint32_t c = 0; c < 4U; ++c) {
                out[d + c] = src[s + c];
            }
        }
    }
    return out;
}

void measure_resample_hypothesis(const std::vector<std::uint8_t>& frame) {
    std::cout << "\n[H2. CPU resample in isolation (no GPU, no cache)]\n";
    std::vector<std::uint8_t> sink;
    const Sample same = time_it(
        [&] { sink = resample_like_cache(frame, k4kWidth, k4kHeight, k4kWidth, k4kHeight); });
    print_row("resample 4K -> 4K", same, frame.size());
    const Sample down =
        time_it([&] { sink = resample_like_cache(frame, k4kWidth, k4kHeight, 1920, 1080); });
    print_row("resample 4K -> 1080p", down, static_cast<std::uint64_t>(1920) * 1080 * 4U);
    if (sink.empty()) {
        std::cout << "(unreachable)\n";
    }
}

// H3: encode cost. A stand-in for the real encoder: pack PathSegment into a flat 32-byte record
// and maintain bounds (the CPU must keep bounds for damage/hit-test without a GPU readback).
// Delta = patch a dirty range and recompute only the dirty chunks' bounds, then merge chunks.
struct PackedSegment {
    std::uint32_t kind;
    float p[6];
    std::uint32_t pad;
};
static_assert(sizeof(PackedSegment) == 32);

constexpr std::size_t kChunk = 1024;

struct Bounds {
    float min_x = 1e30f;
    float min_y = 1e30f;
    float max_x = -1e30f;
    float max_y = -1e30f;
};

void pack_range(const std::vector<path::PathSegment>& in, std::vector<PackedSegment>& out,
                std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        const path::PathSegment& s = in[i];
        out[i] = {static_cast<std::uint32_t>(s.kind),
                  {s.p0.x, s.p0.y, s.p1.x, s.p1.y, s.p2.x, s.p2.y},
                  0};
    }
}

[[nodiscard]] Bounds chunk_bounds(const std::vector<PackedSegment>& out, std::size_t chunk,
                                  std::size_t count) {
    Bounds b;
    const std::size_t begin = chunk * kChunk;
    const std::size_t end = std::min(count, begin + kChunk);
    for (std::size_t i = begin; i < end; ++i) {
        for (int k = 0; k < 6; k += 2) {
            b.min_x = std::min(b.min_x, out[i].p[k]);
            b.max_x = std::max(b.max_x, out[i].p[k]);
            b.min_y = std::min(b.min_y, out[i].p[k + 1]);
            b.max_y = std::max(b.max_y, out[i].p[k + 1]);
        }
    }
    return b;
}

[[nodiscard]] Bounds merge_bounds(const std::vector<Bounds>& chunks) {
    Bounds b;
    for (const Bounds& c : chunks) {
        b.min_x = std::min(b.min_x, c.min_x);
        b.max_x = std::max(b.max_x, c.max_x);
        b.min_y = std::min(b.min_y, c.min_y);
        b.max_y = std::max(b.max_y, c.max_y);
    }
    return b;
}

void measure_encode_hypothesis() {
    std::cout << "\n[H3. CPU encode proxy: pack to 32-byte records + chunked bounds]\n";
    for (const std::size_t count : {std::size_t{1000}, std::size_t{100000}, std::size_t{1000000}}) {
        std::vector<path::PathSegment> in(count);
        std::uint32_t seed = 12345U;
        const auto next = [&seed] {
            seed = seed * 1664525U + 1013904223U;
            return static_cast<float>(seed >> 8) / 16777216.0f * 3840.0f;
        };
        for (path::PathSegment& s : in) {
            s.kind = path::SegmentKind::cubic_to;
            s.p0 = {next(), next()};
            s.p1 = {next(), next()};
            s.p2 = {next(), next()};
        }
        std::vector<PackedSegment> out(count);
        const std::size_t chunks = (count + kChunk - 1) / kChunk;
        std::vector<Bounds> bounds(chunks);
        Bounds total;

        const Sample full = time_it([&] {
            pack_range(in, out, 0, count);
            for (std::size_t c = 0; c < chunks; ++c) {
                bounds[c] = chunk_bounds(out, c, count);
            }
            total = merge_bounds(bounds);
        });
        // 1% contiguous dirty range, at least one chunk.
        const std::size_t dirty = std::max<std::size_t>(kChunk, count / 100);
        const std::size_t first_chunk = chunks / 3;
        const std::size_t begin = std::min(count, first_chunk * kChunk);
        const std::size_t end = std::min(count, begin + dirty);
        const Sample delta = time_it([&] {
            pack_range(in, out, begin, end);
            for (std::size_t c = begin / kChunk; c <= (end - 1) / kChunk; ++c) {
                bounds[c] = chunk_bounds(out, c, count);
            }
            total = merge_bounds(bounds);
        });
        const std::string label = std::to_string(count) + " segments";
        print_row("encode full  " + label, full, count * sizeof(PackedSegment));
        print_row("encode delta " + label + " (1% dirty)", delta,
                  (end - begin) * sizeof(PackedSegment));
        if (total.max_x < total.min_x) {
            std::cout << "(unreachable)\n";
        }
    }
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;

    try {
        const auto device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
        const std::uint64_t bytes_4k = static_cast<std::uint64_t>(k4kWidth) * k4kHeight * 4U;
        std::cout << "cpu_to_gpu_4k: " << k4kWidth << "x" << k4kHeight
                  << " RGBA8 = " << (bytes_4k / (1024.0 * 1024.0)) << " MiB per frame, "
                  << kIterations << " iterations after " << kWarmup << " warm-up\n\n";

        std::vector<std::uint8_t> frame(bytes_4k, 0);
        std::vector<std::uint8_t> staging_probe(bytes_4k, 0);
        double raster_dense_ms = 0.0;
        double raster_sparse_ms = 0.0;

        // 5. Host-bandwidth floor.
        std::cout << "[host floor]\n";
        const Sample memcpy_sample = time_it([&] {
            std::memcpy(staging_probe.data(), frame.data(), frame.size());
            // Keep the optimizer from dropping the copy.
            frame[0] = static_cast<std::uint8_t>(frame[0] + staging_probe[bytes_4k / 2]);
        });
        print_row("memcpy 4K RGBA8 (CPU->CPU)", memcpy_sample, bytes_4k);
        const Sample clear_sample =
            time_it([&] { std::fill(frame.begin(), frame.end(), static_cast<std::uint8_t>(0)); });
        print_row("clear 4K RGBA8", clear_sample, bytes_4k);

        // 1. ThorVG CPU raster.
        std::cout << "\n[1. ThorVG CPU raster 4K]\n";
        const auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
        if (rasterizer == nullptr || rasterizer->backend_name() != "thorvg-cpu") {
            std::cout << "ThorVG backend unavailable in this build; skipping raster rows\n";
        } else {
            std::vector<Scene> scenes;
            scenes.push_back({"sparse (30 shapes, small area)", sparse_scene()});
            scenes.push_back({"dense (13 shapes, full-frame fills)", dense_scene()});
            for (Scene& scene : scenes) {
                const path::RasterTarget target{std::span<std::uint8_t>(frame), k4kWidth, k4kHeight,
                                                k4kWidth * 4U};
                const Sample sample = time_it([&] {
                    std::fill(frame.begin(), frame.end(), static_cast<std::uint8_t>(0));
                    const auto result = rasterizer->rasterize(scene.shapes, target);
                    if (!result.has_value()) {
                        throw std::runtime_error("rasterize failed");
                    }
                });
                print_row(std::string("raster+clear ") + scene.name, sample, bytes_4k);
                (scene.name[0] == 's' ? raster_sparse_ms : raster_dense_ms) = sample.median;
            }
        }

        // 2. RHI upload of the raster buffer.
        std::cout << "\n[2. RHI upload 4K (create_texture / Texture::write)]\n";
        rhi::TextureDesc desc;
        desc.extent = {k4kWidth, k4kHeight};
        desc.pixels = frame.data();
        desc.row_pitch = static_cast<std::size_t>(k4kWidth) * 4U;
        desc.filter = rhi::Filter::Linear;

        std::vector<std::unique_ptr<rhi::Texture>> keep;
        const Sample create_cpu = time_it([&] {
            keep.push_back(device->create_texture(desc));
            if (keep.size() > 4) {
                device->wait_idle();
                keep.clear();
            }
        });
        device->wait_idle();
        keep.clear();
        print_row("create_texture 4K: CPU side (call returns)", create_cpu, bytes_4k);

        const Sample create_done = time_it([&] {
            auto texture = device->create_texture(desc);
            device->wait_idle();
        });
        print_row("create_texture 4K: until GPU has pixels", create_done, bytes_4k);

        auto target_texture = device->create_texture([&] {
            rhi::TextureDesc blank = desc;
            blank.pixels = nullptr;
            return blank;
        }());
        device->wait_idle();
        const Sample write_cpu =
            time_it([&] { target_texture->write(frame.data(), frame.size()); });
        device->wait_idle();
        print_row("Texture::write 4K: CPU side (call returns)", write_cpu, bytes_4k);
        const Sample write_done = time_it([&] {
            target_texture->write(frame.data(), frame.size());
            device->wait_idle();
        });
        print_row("Texture::write 4K: until GPU has pixels", write_done, bytes_4k);

        // 3. Image path.
        std::cout << "\n[3. ImageTextureCache miss (resample_nearest + upload)]\n";
        image::DecodedImage source;
        source.width = source.source_width = k4kWidth;
        source.height = source.source_height = k4kHeight;
        source.format = image::ImageFormat::png;
        source.content_hash = 0xC0FFEEULL;
        source.rgba.assign(bytes_4k, 0x7F);
        image::ImageTextureCacheConfig cache_config;
        cache_config.max_bytes = 512ULL * 1024ULL * 1024ULL;
        image::ImageTextureCache cache(*device, cache_config);

        struct ImageCase {
            const char* name;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t levels;
        };
        std::vector<ImageCase> cases = {
            {"4K -> 4K raster, 1 level", k4kWidth, k4kHeight, 1},
            {"4K -> 1080p raster, 1 level", 1920, 1080, 1},
        };
        if (cache.supports_mipmaps()) {
            cases.push_back({"4K -> 4K raster, full mip chain", k4kWidth, k4kHeight,
                             rhi::full_mip_levels({k4kWidth, k4kHeight})});
        }
        double image_4k_ms = 0.0;
        for (const ImageCase& image_case : cases) {
            image::ImageTextureKey key;
            key.content_hash = source.content_hash;
            key.width = image_case.width;
            key.height = image_case.height;
            key.levels = image_case.levels;
            const Sample sample = time_it([&] {
                cache.invalidate_all();
                const auto result = cache.acquire(source, key);
                if (!result.has_value()) {
                    throw std::runtime_error("image acquire failed");
                }
                device->wait_idle();
            });
            print_row(std::string("image ") + image_case.name + " (to GPU)", sample,
                      static_cast<std::uint64_t>(image_case.width) * image_case.height * 4U);
            if (image_case.levels == 1 && image_case.width == k4kWidth) {
                image_4k_ms = sample.median;
            }
        }

        // 4. Glyph-atlas style page writes.
        std::cout << "\n[4. Whole-page writes (glyph atlas style)]\n";
        for (const std::uint32_t side : {512U, 1024U, 2048U}) {
            rhi::TextureDesc page_desc;
            page_desc.extent = {side, side};
            auto page = device->create_texture(page_desc);
            device->wait_idle();
            std::vector<std::uint8_t> page_pixels(static_cast<std::size_t>(side) * side * 4U, 0x55);
            const Sample sample = time_it([&] {
                page->write(page_pixels.data(), page_pixels.size());
                device->wait_idle();
            });
            print_row("page " + std::to_string(side) + "x" + std::to_string(side) +
                          " write (until GPU)",
                      sample, page_pixels.size());
        }

        // Hypothesis checks behind the issue plan (#681, #682, #683).
        measure_staging_hypothesis(*device, frame);
        measure_resample_hypothesis(frame);
        measure_encode_hypothesis();

        // 6. Per-frame budget summary.
        std::cout << "\n[budget: what one animated 4K vector frame costs on this path]\n";
        const double upload_ms = write_cpu.median;
        if (raster_dense_ms > 0.0) {
            print_budget("dense raster + Texture::write (CPU side)", raster_dense_ms + upload_ms);
        }
        if (raster_sparse_ms > 0.0) {
            print_budget("sparse raster + Texture::write (CPU side)", raster_sparse_ms + upload_ms);
        }
        print_budget("image 4K miss (resample + upload, to GPU)", image_4k_ms);
    } catch (const std::exception& error) {
        std::cerr << "cpu_to_gpu_4k_measure: skipped (" << error.what() << ")\n";
        return 1;
    }
    return 0;
}
