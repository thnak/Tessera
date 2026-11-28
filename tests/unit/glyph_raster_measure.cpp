// SPDX-License-Identifier: MIT
//
// Issue #686 step 1: what CPU glyph rasterization costs today, so a GPU route is judged on numbers.
// Not a test (no assertions, no ctest entry): run it by hand in Release and read the table.
//   glyph_raster_measure <font.ttf>
// Three questions: the cost of one glyph-cache miss by pixel size, the cost of rebuilding the atlas
// when the render scale steps, and the bytes a text column uploads while it scrolls.

#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_prewarm.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace tessera::ui::text;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// The printable ASCII range: 95 glyphs, what a Latin UI string draws from.
constexpr char32_t kFirst = 0x20;
constexpr char32_t kLast = 0x7E;

void miss_cost(GlyphRasterizer& rasterizer) {
    std::cout << "\n[1. Glyph cache miss: FreeType rasterize, 95 ASCII glyphs, median of 9 runs]\n"
              << "  size   gray us/glyph  gray bytes/glyph   lcd us/glyph  lcd bytes/glyph\n";
    for (const std::uint16_t size :
         std::initializer_list<std::uint16_t>{12, 16, 24, 32, 48, 64, 128, 256, 512}) {
        double us[2] = {};
        double bytes[2] = {};
        for (int mode = 0; mode < 2; ++mode) {
            std::vector<double> runs;
            std::uint64_t total = 0;
            for (int run = 0; run < 9; ++run) {
                total = 0;
                const auto start = Clock::now();
                for (char32_t c = kFirst; c <= kLast; ++c) {
                    const GlyphBitmap bitmap = rasterizer.rasterize_glyph(
                        rasterizer.glyph_index(c), size, GlyphHinting::Font, 0,
                        mode == 0 ? GlyphAaMode::Grayscale : GlyphAaMode::Lcd);
                    total += bitmap.coverage.size();
                }
                runs.push_back(ms_since(start));
            }
            us[mode] = median(runs) * 1000.0 / 95.0;
            bytes[mode] = static_cast<double>(total) / 95.0;
        }
        std::cout << std::setw(6) << size << std::fixed << std::setprecision(1) << std::setw(16)
                  << us[0] << std::setw(18) << bytes[0] << std::setw(15) << us[1] << std::setw(17)
                  << bytes[1] << "\n";
    }
}

void rebuild_on_scale_step(GlyphRasterizer& rasterizer) {
    std::cout
        << "\n[2. Atlas rebuild when the render scale steps, 1024x1024 pages]\n"
        << "  A UI draws 4 text sizes (12, 14, 16, 20 CSS px) from the 95 ASCII glyphs. A scale\n"
        << "  step changes every pixel size, so the whole working set is rasterized again.\n"
        << "  scale   glyphs   rasterize ms   pages   full-page upload MiB   dirty-rect MiB\n";
    for (const double scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        std::vector<double> runs;
        std::uint64_t glyphs = 0;
        std::uint64_t full_bytes = 0;
        std::uint64_t dirty_bytes = 0;
        for (int run = 0; run < 5; ++run) {
            GlyphAtlasConfig config;
            config.page_width = 1024;
            config.page_height = 1024;
            config.max_pages = 8;
            GlyphAtlas atlas(rasterizer, config);
            atlas.begin_frame();
            glyphs = 0;
            const auto start = Clock::now();
            for (const double css : {12.0, 14.0, 16.0, 20.0}) {
                const auto px = static_cast<std::uint16_t>(css * scale + 0.5);
                for (char32_t c = kFirst; c <= kLast; ++c) {
                    atlas.get(c, px);
                    ++glyphs;
                }
            }
            runs.push_back(ms_since(start));
            full_bytes = 0;
            dirty_bytes = 0;
            for (std::uint16_t page = 0; page < atlas.page_count(); ++page) {
                full_bytes +=
                    static_cast<std::uint64_t>(atlas.page_width()) * atlas.page_height() * 4U;
                const GlyphAtlasRect rect = atlas.take_dirty(page);
                dirty_bytes += static_cast<std::uint64_t>(rect.width) * rect.height * 4U;
            }
        }
        // Pages are RGBA8 (glyph_atlas.hpp): four bytes a texel however few the glyph needs.
        std::cout << std::fixed << std::setprecision(2) << std::setw(7) << scale << std::setw(9)
                  << glyphs << std::setw(15) << median(runs) << std::setw(8)
                  << (full_bytes / (1024ULL * 1024ULL * 4ULL)) << std::setw(23)
                  << static_cast<double>(full_bytes) / (1024.0 * 1024.0) << std::setw(17)
                  << static_cast<double>(dirty_bytes) / (1024.0 * 1024.0) << "\n";
    }
}

// Issue #948: the same 380-glyph rebuild with the rasterization on a worker. The executor is
// manual so the three costs can be timed apart: what the UI thread pays to hand the job over
// (start), what the worker spends (run_one, off the UI thread in production), and what the UI
// thread pays to place the finished bitmaps (install).
void rebuild_off_thread(GlyphRasterizer& rasterizer) {
    std::cout << "\n[4. The same rebuild with the rasterization on a worker, 1024x1024 pages]\n"
              << "  scale   glyphs   UI start ms   worker ms   UI install ms   UI total ms\n";
    for (const double scale : {1.25, 1.5, 2.0, 3.0}) {
        std::vector<double> starts;
        std::vector<double> works;
        std::vector<double> installs;
        std::uint64_t glyphs = 0;
        for (int run = 0; run < 5; ++run) {
            std::vector<GlyphAtlasKey> keys;
            for (const double css : {12.0, 14.0, 16.0, 20.0}) {
                const auto px = static_cast<std::uint16_t>(css * scale + 0.5);
                for (char32_t c = kFirst; c <= kLast; ++c) {
                    keys.push_back({rasterizer.glyph_index(c), px, 0, 0});
                }
            }
            glyphs = keys.size();
            GlyphAtlasConfig config;
            config.page_width = 1024;
            config.page_height = 1024;
            config.max_pages = 8;
            GlyphAtlas atlas(rasterizer, config);
            tessera::runtime::ManualIoExecutor executor;
            GlyphPrewarm prewarm;
            GlyphRasterizer* stack[] = {&rasterizer};
            auto start = Clock::now();
            prewarm.start(executor, stack, std::move(keys), GlyphHinting::Font,
                          GlyphAaMode::Grayscale);
            starts.push_back(ms_since(start));
            start = Clock::now();
            executor.run_one();
            works.push_back(ms_since(start));
            executor.run_completion_one();
            start = Clock::now();
            prewarm.install(atlas);
            installs.push_back(ms_since(start));
        }
        const double s = median(starts);
        const double i = median(installs);
        std::cout << std::fixed << std::setprecision(2) << std::setw(7) << scale << std::setw(9)
                  << glyphs << std::setprecision(3) << std::setw(14) << s << std::setw(12)
                  << median(works) << std::setw(16) << i << std::setw(14) << s + i << "\n";
    }
}

void scrolling_column(GlyphRasterizer& rasterizer) {
    std::cout
        << "\n[3. Atlas upload while scrolling a text column, 16 px, 512x512 pages]\n"
        << "  The column is 400 lines of printable ASCII (a long log or document). Each frame\n"
        << "  draws the 40 visible lines; scrolling moves the window by one line per frame.\n";
    GlyphAtlasConfig config;
    GlyphAtlas atlas(rasterizer, config);
    std::vector<std::string> lines;
    std::uint32_t state = 12345;
    for (int i = 0; i < 400; ++i) {
        std::string line;
        for (int c = 0; c < 80; ++c) {
            state = state * 1664525U + 1013904223U;
            line.push_back(static_cast<char>(kFirst + (state >> 24) % 95));
        }
        lines.push_back(line);
    }
    std::uint64_t first_frame_bytes = 0;
    std::uint64_t later_bytes = 0;
    std::uint64_t first_misses = 0;
    double first_ms = 0;
    double later_ms = 0;
    for (int frame = 0; frame < 360; ++frame) {
        atlas.begin_frame();
        const std::uint64_t misses_before = atlas.stats().misses;
        const auto start = Clock::now();
        for (int row = frame; row < frame + 40; ++row) {
            for (const char ch : lines[static_cast<std::size_t>(row)]) {
                atlas.get(static_cast<char32_t>(ch), 16);
            }
        }
        const double ms = ms_since(start);
        std::uint64_t bytes = 0;
        for (std::uint16_t page = 0; page < atlas.page_count(); ++page) {
            const GlyphAtlasRect rect = atlas.take_dirty(page);
            bytes += static_cast<std::uint64_t>(rect.width) * rect.height * 4U;
        }
        if (frame == 0) {
            first_frame_bytes = bytes;
            first_misses = atlas.stats().misses - misses_before;
            first_ms = ms;
        } else {
            later_bytes += bytes;
            later_ms += ms;
        }
    }
    std::cout << std::fixed << std::setprecision(3) << "  frame 0 (cold): " << first_misses
              << " misses, " << first_ms << " ms, " << first_frame_bytes << " bytes uploaded\n"
              << "  frames 1-359: " << later_bytes << " bytes uploaded in total, "
              << later_ms / 359.0 << " ms per frame to look up 3200 glyphs\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: glyph_raster_measure <font.ttf>\n";
        return 1;
    }
    auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "cannot load font: " << loaded.error().message << "\n";
        return 1;
    }
    GlyphRasterizer rasterizer(std::move(*loaded));
    std::cout << "glyph_raster_measure: FreeType on one CPU thread, " << argv[1] << "\n";
    miss_cost(rasterizer);
    rebuild_on_scale_step(rasterizer);
    rebuild_off_thread(rasterizer);
    scrolling_column(rasterizer);
    return 0;
}
