// SPDX-License-Identifier: MIT

#include "downscale_kernels.hpp"

#include <algorithm>
#include <vector>

namespace tessera::rendering::image {

namespace {

// 255 * 255 * 65536 < 2^32 (and leaves room for the rounding term), so a box of at most this many
// texels sums in 32 bits. Larger boxes (a source reduced more than 256x on both axes) take the
// 64-bit loop; they are rare and their cost is dominated by reading the source anyway.
constexpr std::uint64_t kMaxSimdBoxArea = 65536;

// Source columns one tile of the SIMD path covers (a tile also grows to hold a single wider span).
constexpr std::uint32_t kTilePixels = 1024;

struct Span {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

// Source span of output index `o`: integer bounds, at least one texel wide, one rule for both axes.
std::vector<Span> axis_spans(std::uint32_t length, std::uint32_t count) {
    std::vector<Span> spans(count);
    for (std::uint32_t o = 0; o < count; ++o) {
        const auto begin =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(o) * length / count);
        const auto end = std::max(
            begin + 1, static_cast<std::uint32_t>(
                           (static_cast<std::uint64_t>(o + 1) * length + count - 1) / count));
        spans[o] = {begin, std::min(end, length)};
    }
    return spans;
}

std::uint32_t max_width(const std::vector<Span>& spans) {
    std::uint32_t widest = 0;
    for (const Span& s : spans) {
        widest = std::max(widest, s.end - s.begin);
    }
    return widest;
}

AccumulateRowFn kernel_for(DownscaleIsa isa) noexcept {
#if TESSERA_DOWNSCALE_X86
    switch (isa) {
    case DownscaleIsa::avx512:
        return accumulate_row_avx512;
    case DownscaleIsa::avx2:
        return accumulate_row_avx2;
    case DownscaleIsa::sse2:
        return accumulate_row_sse2;
    case DownscaleIsa::portable:
        break;
    }
#else
    (void)isa;
#endif
    return [](const std::uint8_t* row, std::uint32_t* planes, std::size_t stride,
              std::size_t pixels) { accumulate_row_scalar(row, planes, stride, 0, pixels); };
}

std::uint8_t* start_output(DecodedImage& out, const DecodedImage& source, std::uint32_t width,
                           std::uint32_t height) {
    out.format = source.format;
    out.content_hash = source.content_hash;
    out.source_width = source.source_width != 0 ? source.source_width : source.width;
    out.source_height = source.source_height != 0 ? source.source_height : source.height;
    if (width == 0 || height == 0 || source.width == 0 || source.height == 0) {
        return nullptr;
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    return out.rgba.data();
}

// The 64-bit loop for boxes too large for 32-bit sums.
void reduce_wide(const DecodedImage& source, std::uint32_t width, std::uint32_t height,
                 const std::vector<Span>& columns, const std::vector<Span>& rows,
                 std::uint8_t* out) {
    for (std::uint32_t oy = 0; oy < height; ++oy) {
        for (std::uint32_t ox = 0; ox < width; ++ox, out += 4) {
            std::uint64_t sum_r = 0;
            std::uint64_t sum_g = 0;
            std::uint64_t sum_b = 0;
            std::uint64_t sum_a = 0;
            for (std::uint32_t y = rows[oy].begin; y < rows[oy].end; ++y) {
                const std::uint8_t* p =
                    &source.rgba[(static_cast<std::size_t>(y) * source.width + columns[ox].begin) *
                                 4U];
                for (std::uint32_t x = columns[ox].begin; x < columns[ox].end; ++x, p += 4) {
                    sum_r += static_cast<std::uint64_t>(p[0]) * p[3];
                    sum_g += static_cast<std::uint64_t>(p[1]) * p[3];
                    sum_b += static_cast<std::uint64_t>(p[2]) * p[3];
                    sum_a += p[3];
                }
            }
            const std::uint64_t count = static_cast<std::uint64_t>(rows[oy].end - rows[oy].begin) *
                                        (columns[ox].end - columns[ox].begin);
            if (sum_a == 0) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }
            out[0] = static_cast<std::uint8_t>((sum_r + sum_a / 2) / sum_a);
            out[1] = static_cast<std::uint8_t>((sum_g + sum_a / 2) / sum_a);
            out[2] = static_cast<std::uint8_t>((sum_b + sum_a / 2) / sum_a);
            out[3] = static_cast<std::uint8_t>((sum_a + count / 2) / count);
        }
    }
}

} // namespace

const char* isa_name(DownscaleIsa isa) noexcept {
    switch (isa) {
    case DownscaleIsa::portable:
        return "portable";
    case DownscaleIsa::sse2:
        return "sse2";
    case DownscaleIsa::avx2:
        return "avx2";
    case DownscaleIsa::avx512:
        return "avx512";
    }
    return "portable";
}

bool isa_supported(DownscaleIsa isa) noexcept {
    switch (isa) {
    case DownscaleIsa::portable:
        return true;
#if TESSERA_DOWNSCALE_X86
    case DownscaleIsa::sse2:
        return true; // part of the x86-64 baseline
    case DownscaleIsa::avx2:
        return x86_avx2_available();
    case DownscaleIsa::avx512:
        return x86_avx512_available();
#else
    default:
        return false;
#endif
    }
    return false;
}

DownscaleIsa best_isa() noexcept {
    static const DownscaleIsa best = [] {
        for (const DownscaleIsa isa :
             {DownscaleIsa::avx512, DownscaleIsa::avx2, DownscaleIsa::sse2}) {
            if (isa_supported(isa)) {
                return isa;
            }
        }
        return DownscaleIsa::portable;
    }();
    return best;
}

void accumulate_row_scalar(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                           std::size_t first, std::size_t pixels) noexcept {
    for (std::size_t i = first; i < pixels; ++i) {
        const std::uint8_t* p = rgba_row + i * 4;
        const std::uint32_t a = p[3];
        planes[i] += p[0] * a;
        planes[stride + i] += p[1] * a;
        planes[2 * stride + i] += p[2] * a;
        planes[3 * stride + i] += a;
    }
}

DecodedImage downscale_box_with(DownscaleIsa isa, const DecodedImage& source, std::uint32_t width,
                                std::uint32_t height) {
    DecodedImage result;
    std::uint8_t* out = start_output(result, source, width, height);
    if (out == nullptr) {
        return result;
    }
    const std::vector<Span> columns = axis_spans(source.width, width);
    const std::vector<Span> rows = axis_spans(source.height, height);
    if (static_cast<std::uint64_t>(max_width(columns)) * max_width(rows) > kMaxSimdBoxArea) {
        reduce_wide(source, width, height, columns, rows, out);
        return result;
    }

    // Per output row and per tile of output columns: add each source row of the box into four
    // planar accumulators (SIMD, one pass over the tile's source columns), then reduce each output
    // pixel's span of those planes. A tile covers at most kTilePixels source columns so the planes
    // stay in L1 (4 planes * 4 bytes * 1024 = 16 KiB); whole-row planes spilled to L2 and cost as
    // much as the arithmetic. The sums are the ones the plain nested loop forms, so every kernel
    // and every tile size gives identical bytes.
    const AccumulateRowFn accumulate = isa_supported(isa) ? kernel_for(isa) : kernel_for({});
    struct Tile {
        std::uint32_t first_output = 0;
        std::uint32_t last_output = 0; // exclusive
        std::uint32_t begin = 0;       // source columns [begin, end)
        std::uint32_t end = 0;
    };
    std::vector<Tile> tiles;
    std::size_t capacity = 0;
    for (std::uint32_t ox = 0; ox < width;) {
        Tile tile{ox, ox + 1, columns[ox].begin, columns[ox].end};
        while (tile.last_output < width &&
               columns[tile.last_output].end - tile.begin <= kTilePixels) {
            tile.end = std::max(tile.end, columns[tile.last_output].end);
            ++tile.last_output;
        }
        capacity = std::max<std::size_t>(capacity, tile.end - tile.begin);
        ox = tile.last_output;
        tiles.push_back(tile);
    }
    const std::size_t stride = (capacity + 15U) & ~std::size_t{15};
    std::vector<std::uint32_t> planes(stride * 4U);
    for (std::uint32_t oy = 0; oy < height; ++oy, out += static_cast<std::size_t>(width) * 4U) {
        const std::uint32_t box_rows = rows[oy].end - rows[oy].begin;
        for (const Tile& tile : tiles) {
            const std::size_t length = tile.end - tile.begin;
            for (std::size_t c = 0; c < 4; ++c) {
                std::fill_n(planes.begin() + static_cast<std::ptrdiff_t>(c * stride), length, 0U);
            }
            for (std::uint32_t y = rows[oy].begin; y < rows[oy].end; ++y) {
                accumulate(
                    &source.rgba[(static_cast<std::size_t>(y) * source.width + tile.begin) * 4U],
                    planes.data(), stride, length);
            }
            const std::uint32_t* pr = planes.data() - tile.begin;
            const std::uint32_t* pg = pr + stride;
            const std::uint32_t* pb = pg + stride;
            const std::uint32_t* pa = pb + stride;
            std::uint8_t* q = out + static_cast<std::size_t>(tile.first_output) * 4U;
            for (std::uint32_t ox = tile.first_output; ox < tile.last_output; ++ox, q += 4) {
                std::uint32_t sum_r = 0;
                std::uint32_t sum_g = 0;
                std::uint32_t sum_b = 0;
                std::uint32_t sum_a = 0;
                for (std::uint32_t x = columns[ox].begin; x < columns[ox].end; ++x) {
                    sum_r += pr[x];
                    sum_g += pg[x];
                    sum_b += pb[x];
                    sum_a += pa[x];
                }
                if (sum_a == 0) {
                    q[0] = q[1] = q[2] = q[3] = 0;
                    continue;
                }
                const std::uint32_t count = box_rows * (columns[ox].end - columns[ox].begin);
                q[0] = static_cast<std::uint8_t>((sum_r + sum_a / 2) / sum_a);
                q[1] = static_cast<std::uint8_t>((sum_g + sum_a / 2) / sum_a);
                q[2] = static_cast<std::uint8_t>((sum_b + sum_a / 2) / sum_a);
                q[3] = static_cast<std::uint8_t>((sum_a + count / 2) / count);
            }
        }
    }
    return result;
}

} // namespace tessera::rendering::image
