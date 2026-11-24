// SPDX-License-Identifier: MIT
//
// Hypothesis checks of the 4K measurement tool (#681, #682, #683): staging-allocation spikes (H1),
// the CPU resample loop on its own (H2), GPU nearest versus the CPU rule (H2b) and the CPU encode
// proxy (H3).

#include "cpu_to_gpu_4k_measure.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_resize.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"

#include <array>
#include <cstdlib>
#include <cstring>

namespace measure {

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

void measure_downscale_box(const std::vector<std::uint8_t>& source_4k) {
    std::cout << "\n[H6. The image path's decode-target downscale (downscale_box), CPU only]\n";
    // The real function, not a copy: the whole question is what the code the image path calls
    // costs, and a reimplementation here could be faster or slower than the thing being judged.
    image::DecodedImage source;
    source.width = k4kWidth;
    source.height = k4kHeight;
    source.source_width = k4kWidth;
    source.source_height = k4kHeight;
    source.format = image::ImageFormat::png;
    source.rgba = source_4k;

    struct Case {
        const char* name;
        std::uint32_t width;
        std::uint32_t height;
    };
    // 1080p is the common "4K image on a 1080p canvas" case; 540p is the 2x-downscale a thumbnail
    // or an offscreen preview asks for. Both are in the issue, and a third would not change the
    // answer.
    const Case cases[] = {
        {"downscale_box 4K -> 1080p", 1920, 1080},
        {"downscale_box 4K -> 540p", 960, 540},
    };
    for (const Case& test_case : cases) {
        std::size_t out_bytes = 0;
        const Sample sample = time_it([&] {
            const image::DecodedImage reduced =
                image::downscale_box(source, test_case.width, test_case.height);
            out_bytes = reduced.rgba.size();
        });
        // Reported against the OUTPUT bytes, so the GB/s is the rate it produces a reduced image -
        // which is what an upload would then cost on top. The input is always 4K, so the same row
        // also says how much work the box filter does per output pixel.
        print_row(test_case.name, sample, out_bytes);
        print_budget(test_case.name, sample.median);
    }
    // The floor this has to be judged against: moving the same bytes with no arithmetic at all.
    // Without it "x GB/s" says nothing about whether the loop is slow.
    const Sample copy = time_it([&] {
        std::vector<std::uint8_t> sink(source_4k.size());
        std::memcpy(sink.data(), source_4k.data(), source_4k.size());
    });
    print_row("memcpy 4K (the host-bandwidth floor)", copy, source_4k.size());
}

namespace {

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

} // namespace

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

namespace {

// H2b (#682): does the GPU's nearest sampling reproduce the CPU resample rule bit-exactly? The
// source is random noise so any off-by-one neighbor shows. Ratios of exactly 2:1 and 3:1 put the
// sample position exactly on a texel boundary, which is where float interpolation decides a tie.
[[nodiscard]] std::vector<std::uint8_t> gpu_draw_nearest(rhi::Device& device, rhi::Texture& texture,
                                                         std::uint32_t w, std::uint32_t h) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {w, h};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    auto target = device.create_render_target(target_desc);
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = target_desc.format;
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    tessera::rendering::gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_texture({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, texture,
                          {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(w) * h * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

} // namespace

void measure_nearest_exactness(rhi::Device& device) {
    std::cout << "\n[H2b. GPU nearest vs CPU resample rule: byte-exact?]\n";
    constexpr std::uint32_t kSrcW = 240;
    constexpr std::uint32_t kSrcH = 144;
    std::vector<std::uint8_t> source(static_cast<std::size_t>(kSrcW) * kSrcH * 4U);
    std::uint32_t seed = 987654321U;
    for (std::size_t i = 0; i < source.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        source[i] = (i % 4U == 3U) ? std::uint8_t{255} : static_cast<std::uint8_t>(seed >> 24);
    }
    rhi::TextureDesc desc;
    desc.extent = {kSrcW, kSrcH};
    desc.pixels = source.data();
    desc.row_pitch = static_cast<std::size_t>(kSrcW) * 4U;
    desc.filter = rhi::Filter::Nearest;
    auto texture = device.create_texture(desc);
    device.wait_idle();

    struct Case {
        const char* name;
        std::uint32_t w;
        std::uint32_t h;
    };
    const Case cases[] = {
        {"1:1", 240, 144},
        {"2:1 (tie on boundary)", 120, 72},
        {"3:1 (no tie)", 80, 48},
        {"3:2", 160, 96},
        {"1:2 upscale", 480, 288},
        {"fractional 240->173, 144->97", 173, 97},
        {"fractional 240->199, 144->131", 199, 131},
    };
    for (const Case& c : cases) {
        const auto cpu = resample_like_cache(source, kSrcW, kSrcH, c.w, c.h);
        const auto gpu = gpu_draw_nearest(device, *texture, c.w, c.h);
        std::size_t different = 0;
        int max_diff = 0;
        for (std::size_t i = 0; i < cpu.size(); ++i) {
            if (cpu[i] != gpu[i]) {
                ++different;
                max_diff = std::max(max_diff, std::abs(static_cast<int>(cpu[i]) - gpu[i]));
            }
        }
        const std::size_t pixels = static_cast<std::size_t>(c.w) * c.h;
        std::cout << std::left << std::setw(34) << c.name << std::right << " " << std::setw(4)
                  << c.w << "x" << std::setw(4) << std::left << c.h << std::right
                  << " differing bytes=" << std::setw(7) << different << " of " << cpu.size()
                  << "  max channel diff=" << max_diff
                  << (different == 0 ? "  EXACT" : "  MISMATCH") << " (" << pixels << " px)\n";
    }
}

} // namespace measure
