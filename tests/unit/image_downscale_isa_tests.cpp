// SPDX-License-Identifier: MIT
//
// Every instruction-set kernel of the box downscale (issue #689) must produce the bytes of the
// plain nested loop. The reference below is that loop, kept here on purpose: it is the definition
// the kernels are held to, independent of the code under test. `--bench` prints per-set timings.

#include "downscale_kernels.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

namespace image = tessera::rendering::image;

image::DecodedImage reference(const image::DecodedImage& source, std::uint32_t width,
                              std::uint32_t height) {
    image::DecodedImage out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t oy = 0; oy < height; ++oy) {
        const auto y0 =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(oy) * source.height / height);
        const auto y1 =
            std::max(y0 + 1, static_cast<std::uint32_t>(
                                 (static_cast<std::uint64_t>(oy + 1) * source.height + height - 1) /
                                 height));
        for (std::uint32_t ox = 0; ox < width; ++ox) {
            const auto x0 =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(ox) * source.width / width);
            const auto x1 = std::max(
                x0 + 1,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(ox + 1) * source.width + width - 1) / width));
            std::uint64_t r = 0;
            std::uint64_t g = 0;
            std::uint64_t b = 0;
            std::uint64_t a = 0;
            for (std::uint32_t y = y0; y < y1 && y < source.height; ++y) {
                for (std::uint32_t x = x0; x < x1 && x < source.width; ++x) {
                    const std::uint8_t* p =
                        &source.rgba[(static_cast<std::size_t>(y) * source.width + x) * 4U];
                    r += static_cast<std::uint64_t>(p[0]) * p[3];
                    g += static_cast<std::uint64_t>(p[1]) * p[3];
                    b += static_cast<std::uint64_t>(p[2]) * p[3];
                    a += p[3];
                }
            }
            const std::uint64_t count =
                static_cast<std::uint64_t>(std::min(y1, source.height) - y0) *
                (std::min(x1, source.width) - x0);
            std::uint8_t* q = &out.rgba[(static_cast<std::size_t>(oy) * width + ox) * 4U];
            if (a == 0) {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            q[0] = static_cast<std::uint8_t>((r + a / 2) / a);
            q[1] = static_cast<std::uint8_t>((g + a / 2) / a);
            q[2] = static_cast<std::uint8_t>((b + a / 2) / a);
            q[3] = static_cast<std::uint8_t>((a + count / 2) / count);
        }
    }
    return out;
}

enum class Fill { noise, opaque, mixed_alpha, all_transparent };

image::DecodedImage make_source(std::uint32_t width, std::uint32_t height, Fill fill) {
    image::DecodedImage img;
    img.width = width;
    img.height = height;
    img.source_width = width;
    img.source_height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    // A plain LCG, not <random>: identical bytes on every standard library.
    std::uint32_t state = 0x9e3779b9U ^ (width * 31U + height);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            state = state * 1664525U + 1013904223U;
            img.rgba[i + c] = static_cast<std::uint8_t>(state >> 24);
        }
        switch (fill) {
        case Fill::noise:
            break;
        case Fill::opaque:
            img.rgba[i + 3] = 255;
            break;
        case Fill::mixed_alpha:
            // Extremes are the cases a multiply can get wrong: 0, 255, and the odd middle.
            img.rgba[i + 3] = (i / 4) % 5 == 0 ? 0 : ((i / 4) % 5 == 1 ? 255 : img.rgba[i + 3]);
            break;
        case Fill::all_transparent:
            img.rgba[i + 3] = 0;
            break;
        }
    }
    return img;
}

bool check_all() {
    struct Size {
        std::uint32_t sw, sh, dw, dh;
    };
    // Widths around every vector width (4, 8, 16) and its tail, a non-integer ratio, 1x1, upscale
    // (boxes of one texel) and a box over the 32-bit limit (300x300 -> 1x1) for the wide loop.
    const Size sizes[] = {{1, 1, 1, 1},
                          {3, 5, 1, 1},
                          {4, 4, 2, 2},
                          {7, 9, 3, 4},
                          {8, 8, 4, 4},
                          {15, 6, 7, 3},
                          {16, 16, 8, 8},
                          {17, 17, 5, 6},
                          {31, 12, 13, 5},
                          {33, 7, 11, 7},
                          {64, 48, 23, 17},
                          {100, 100, 99, 99},
                          {40, 30, 64, 48},
                          {257, 3, 129, 2},
                          {300, 300, 1, 1},
                          {301, 299, 2, 1},
                          // Wider than one 1024-column tile: several tiles, tile edges inside a
                          // non-integer ratio, and a single span wider than a tile (3000 -> 1).
                          {2500, 4, 1111, 3},
                          {4100, 5, 2051, 2},
                          {3000, 21, 1, 1}};
    const Fill fills[] = {Fill::noise, Fill::opaque, Fill::mixed_alpha, Fill::all_transparent};
    bool ok = true;
    for (const image::DownscaleIsa isa : {image::DownscaleIsa::portable, image::DownscaleIsa::sse2,
                                          image::DownscaleIsa::avx2, image::DownscaleIsa::avx512}) {
        if (!image::isa_supported(isa)) {
            std::cout << "image_downscale_isa: " << image::isa_name(isa)
                      << " not supported here, skipped\n";
            continue;
        }
        std::size_t cases = 0;
        for (const Size& s : sizes) {
            for (const Fill fill : fills) {
                const image::DecodedImage src = make_source(s.sw, s.sh, fill);
                const image::DecodedImage want = reference(src, s.dw, s.dh);
                const image::DecodedImage got = image::downscale_box_with(isa, src, s.dw, s.dh);
                ++cases;
                if (got.width != s.dw || got.height != s.dh || got.rgba != want.rgba) {
                    std::cerr << "image_downscale_isa: " << image::isa_name(isa) << " differs on "
                              << s.sw << "x" << s.sh << " -> " << s.dw << "x" << s.dh << " fill "
                              << static_cast<int>(fill) << "\n";
                    ok = false;
                }
            }
        }
        std::cout << "image_downscale_isa: " << image::isa_name(isa) << " " << cases
                  << " cases byte-exact\n";
    }
    // The public entry point runs the best set and matches too.
    const image::DecodedImage src = make_source(64, 48, Fill::mixed_alpha);
    ok = (image::downscale_box(src, 23, 17).rgba == reference(src, 23, 17).rgba) && ok;
    // Empty targets stay empty on every set.
    ok = image::downscale_box_with(image::best_isa(), src, 0, 4).rgba.empty() && ok;
    return ok;
}

void bench() {
    for (const Fill fill : {Fill::mixed_alpha, Fill::opaque, Fill::all_transparent}) {
        std::cout << "fill " << static_cast<int>(fill) << "\n";
        const image::DecodedImage src = make_source(3840, 2160, fill);
        for (const image::DownscaleIsa isa :
             {image::DownscaleIsa::portable, image::DownscaleIsa::sse2, image::DownscaleIsa::avx2,
              image::DownscaleIsa::avx512}) {
            if (!image::isa_supported(isa)) {
                continue;
            }
            for (const auto [w, h] : {std::pair{1920U, 1080U}, std::pair{960U, 540U}}) {
                double best = 1e30;
                for (int i = 0; i < 15; ++i) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto out = image::downscale_box_with(isa, src, w, h);
                    const auto t1 = std::chrono::steady_clock::now();
                    if (out.rgba.empty()) {
                        return;
                    }
                    best =
                        std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
                }
                std::cout << image::isa_name(isa) << " 4K -> " << w << "x" << h << ": " << best
                          << " ms\n";
            }
        }
    }
}

#if TESSERA_DOWNSCALE_X86
// The row kernel alone on data that stays in L1: separates what the instruction set does from what
// the memory system and the rest of the downscale do around it.
void bench_kernels() {
    constexpr std::size_t kPixels = 1024;
    constexpr std::size_t kRepeats = 200000;
    const image::DecodedImage row = make_source(kPixels, 1, Fill::mixed_alpha);
    std::vector<std::uint32_t> planes(kPixels * 4U);
    struct Entry {
        image::DownscaleIsa isa;
        image::AccumulateRowFn fn;
    };
    const Entry entries[] = {{image::DownscaleIsa::sse2, image::accumulate_row_sse2},
                             {image::DownscaleIsa::avx2, image::accumulate_row_avx2},
                             {image::DownscaleIsa::avx512, image::accumulate_row_avx512}};
    for (const Entry& e : entries) {
        if (!image::isa_supported(e.isa)) {
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < kRepeats; ++i) {
            e.fn(row.rgba.data(), planes.data(), kPixels, kPixels);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        std::uint32_t checksum = 0;
        for (const std::uint32_t v : planes) {
            checksum = checksum * 31U + v;
        }
        std::cout << "kernel " << image::isa_name(e.isa) << ": " << ns / kRepeats / kPixels
                  << " ns/pixel (L1-resident, checksum " << checksum << ")\n";
        std::fill(planes.begin(), planes.end(), 0U);
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        bench();
#if TESSERA_DOWNSCALE_X86
        bench_kernels();
#endif
        return 0;
    }
    if (!check_all()) {
        return 1;
    }
    std::cout << "image_downscale_isa: ok\n";
    return 0;
}
