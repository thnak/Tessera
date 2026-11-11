// SPDX-License-Identifier: MIT
//
// The production emoji face (issue #12): Twemoji Mozilla, a COLRv0 font, behind Noto Sans in a
// FallbackShaper stack. Emoji, flags and ZWJ sequences must fall through to it, shape to a single
// glyph per cluster, and rasterize to colored (not white, not coverage-only) atlas entries.
// argv[1] is NotoSans, argv[2] is Twemoji.Mozilla.ttf.

#include "tessera/ui/text/fallback_shaper.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>

namespace {

using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "emoji_font: " << message << "\n";
    }
    return condition;
}

bool colored(const GlyphAtlas& atlas, const tessera::ui::text::GlyphAtlasEntry& entry) {
    const auto pixels = atlas.page_pixels(entry.page);
    std::uint32_t opaque = 0;
    std::uint32_t non_gray = 0;
    for (std::uint32_t y = 0; y < entry.height; ++y) {
        for (std::uint32_t x = 0; x < entry.width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(entry.y + y) * atlas.page_width() + entry.x + x) * 4;
            if (pixels[i + 3] > 200) {
                ++opaque;
                const int spread = std::max({pixels[i], pixels[i + 1], pixels[i + 2]}) -
                                   std::min({pixels[i], pixels[i + 1], pixels[i + 2]});
                if (spread > 40) {
                    ++non_gray;
                }
            }
        }
    }
    return opaque > 50 && non_gray * 4 > opaque;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "emoji_font: need the NotoSans and Twemoji paths\n";
        return 64;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto emoji = FontFace::load_from_file(argv[2]);
    if (!sans || !emoji) {
        std::cerr << "emoji_font: cannot load the fonts\n";
        return 65;
    }
    GlyphRasterizer sans_raster(*sans);
    GlyphRasterizer emoji_raster(*emoji);
    const FallbackShaper shaper({*sans, *emoji});

    // "a", grinning face, flag FR, family (ZWJ), thumbs up + skin tone, "b".
    const auto run =
        shaper.shape("a\xF0\x9F\x98\x80"
                     "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"
                     "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
                     "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"
                     "b",
                     TextStyle{});
    bool ok = expect(run.missing_glyphs == 0, "no cluster falls to tofu");
    ok = expect(run.glyphs.size() == 6, "one glyph per cluster (ligatures applied)") && ok;
    if (run.glyphs.size() != 6) {
        std::cerr << "emoji_font: got " << run.glyphs.size() << " glyphs\n";
        return 1;
    }
    ok = expect(run.glyphs[0].face == 0 && run.glyphs[5].face == 0, "letters stay in Noto Sans") &&
         ok;
    GlyphAtlas atlas({&sans_raster, &emoji_raster});
    for (std::size_t i = 1; i <= 4; ++i) {
        ok = expect(run.glyphs[i].face == 1, "emoji cluster uses the emoji face") && ok;
        const auto& entry = atlas.get_glyph(run.glyphs[i].id, 48, run.glyphs[i].face);
        ok = expect(entry.has_bitmap && entry.color, "emoji entry is a color bitmap") && ok;
        ok = expect(colored(atlas, entry), "emoji entry carries real colors") && ok;
    }
    if (!ok) {
        return 1;
    }
    std::cout << "emoji_font: ok\n";
    return 0;
}
