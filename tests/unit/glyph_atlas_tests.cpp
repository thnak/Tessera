// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasConfig;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glyph_atlas: " << message << "\n";
    }
    return condition;
}

bool overlaps(const GlyphAtlasEntry& lhs, const GlyphAtlasEntry& rhs) {
    return lhs.x < rhs.x + rhs.width && rhs.x < lhs.x + lhs.width && lhs.y < rhs.y + rhs.height &&
           rhs.y < lhs.y + lhs.height;
}

bool same_entry(const GlyphAtlasEntry& lhs, const GlyphAtlasEntry& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height &&
           lhs.bearing_x == rhs.bearing_x && lhs.bearing_y == rhs.bearing_y &&
           lhs.advance == rhs.advance && lhs.page == rhs.page && lhs.has_bitmap == rhs.has_bitmap;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "glyph_atlas: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "glyph_atlas: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    GlyphRasterizer rasterizer(*loaded);
    GlyphAtlasConfig config;
    config.page_width = 64;
    config.page_height = 64;
    config.max_pages = 2;
    config.padding = 1;

    GlyphAtlas atlas(rasterizer, config);
    atlas.begin_frame();

    const GlyphAtlasEntry a = atlas.get(U'A', 16);
    if (!expect(a.has_bitmap, "A is packed") || !expect(a.width > 0 && a.height > 0, "A size") ||
        !expect(a.page < config.max_pages, "A page index") ||
        !expect(a.x + a.width <= config.page_width && a.y + a.height <= config.page_height,
                "A inside page") ||
        !expect(atlas.find(U'A', 16) != nullptr, "A is cached")) {
        return 2;
    }

    const std::uint64_t hits = atlas.stats().hits;
    (void)atlas.get(U'A', 16);
    if (!expect(atlas.stats().hits == hits + 1, "second get is a hit")) {
        return 3;
    }

    const GlyphAtlasEntry space = atlas.get(U' ', 16);
    if (!expect(!space.has_bitmap && space.width == 0, "space has no bitmap") ||
        !expect(space.advance > 0.0f, "space keeps advance")) {
        return 4;
    }

    std::vector<GlyphAtlasEntry> packed;
    for (char32_t codepoint = U'A'; codepoint <= U'Z'; ++codepoint) {
        const GlyphAtlasEntry entry = atlas.get(codepoint, 16);
        if (entry.has_bitmap) {
            packed.push_back(entry);
        }
    }
    if (!expect(!packed.empty(), "packed some glyphs") ||
        !expect(atlas.page_count() <= config.max_pages, "page cap")) {
        return 5;
    }
    for (std::size_t i = 0; i < packed.size(); ++i) {
        if (!expect(packed[i].page < atlas.page_count(), "glyph page in range")) {
            return 6;
        }
        for (std::size_t j = i + 1; j < packed.size(); ++j) {
            if (packed[i].page == packed[j].page &&
                !expect(!overlaps(packed[i], packed[j]), "packed glyphs do not overlap")) {
                return 7;
            }
        }
    }

    const std::uint16_t first_page = packed[0].page;
    const std::uint32_t generation = atlas.page_generation(first_page);
    const std::span<const std::uint8_t> pixels = atlas.page_pixels(first_page);
    if (!expect(generation > 0, "page generation advances") ||
        !expect(pixels.size() ==
                    static_cast<std::size_t>(config.page_width) * config.page_height * 4,
                "page pixel size")) {
        return 8;
    }
    bool ink = false;
    for (std::uint32_t row = 0; row < packed[0].height; ++row) {
        for (std::uint32_t column = 0; column < packed[0].width; ++column) {
            const std::size_t index =
                ((static_cast<std::size_t>(packed[0].y + row) * config.page_width) + packed[0].x +
                 column) *
                4;
            ink = ink || pixels[index + 3] != 0;
        }
    }
    if (!expect(ink, "page contains coverage")) {
        return 9;
    }

    GlyphAtlas deterministic(rasterizer, config);
    deterministic.begin_frame();
    for (char32_t codepoint = U'A'; codepoint <= U'Z'; ++codepoint) {
        const GlyphAtlasEntry entry = deterministic.get(codepoint, 16);
        const GlyphAtlasEntry* expected = atlas.find(codepoint, 16);
        if (!expect(entry.has_bitmap == (expected != nullptr && expected->has_bitmap),
                    "deterministic cache presence")) {
            return 10;
        }
        if (expected != nullptr && entry.has_bitmap &&
            !expect(same_entry(entry, *expected), "deterministic placement")) {
            return 11;
        }
    }
    for (std::uint16_t page = 0; page < atlas.page_count(); ++page) {
        const std::span<const std::uint8_t> lhs = atlas.page_pixels(page);
        const std::span<const std::uint8_t> rhs = deterministic.page_pixels(page);
        if (!expect(lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin()),
                    "deterministic page pixels")) {
            return 12;
        }
    }

    GlyphAtlasConfig tight;
    tight.page_width = 32;
    tight.page_height = 32;
    tight.max_pages = 1;
    tight.padding = 1;
    GlyphAtlas evicting(rasterizer, tight);
    evicting.begin_frame();
    for (char32_t codepoint = U'A'; codepoint <= U'Z'; ++codepoint) {
        (void)evicting.get(codepoint, 20);
    }
    if (!expect(evicting.stats().dropped > 0, "full used page cannot evict")) {
        return 13;
    }
    const std::uint64_t evictions = evicting.stats().evictions;
    evicting.begin_frame();
    const GlyphAtlasEntry z = evicting.get(U'z', 20);
    if (!expect(evicting.stats().evictions > evictions, "next frame evicts LRU page") ||
        !expect(z.has_bitmap, "evicted page accepts the new glyph")) {
        return 14;
    }

    std::cout << "glyph_atlas: ok\n";
    return 0;
}
