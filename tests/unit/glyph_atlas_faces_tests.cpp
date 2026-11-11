// SPDX-License-Identifier: MIT
//
// Face-aware glyph atlas (issue #12 slice b): entries are keyed by (face, glyph, size), each face
// is rasterized by its own rasterizer, and a face outside the stack is an empty entry. argv[1] is
// NotoSans, argv[2] is NotoSansHebrew.

#include "tessera/ui/text/fallback_shaper.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"

#include <iostream>
#include <string>

namespace {

using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glyph_atlas_faces: " << message << "\n";
    }
    return condition;
}

bool has_ink(const GlyphAtlas& atlas, const GlyphAtlasEntry& entry) {
    const auto pixels = atlas.page_pixels(entry.page);
    for (std::uint32_t y = 0; y < entry.height; ++y) {
        for (std::uint32_t x = 0; x < entry.width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(entry.y + y) * atlas.page_width() + entry.x + x) * 4;
            if (pixels[at + 3] != 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "glyph_atlas_faces: need the NotoSans and NotoSansHebrew paths\n";
        return 64;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto hebrew = FontFace::load_from_file(argv[2]);
    if (!sans || !hebrew) {
        std::cerr << "glyph_atlas_faces: cannot load the fonts\n";
        return 65;
    }

    GlyphRasterizer sans_raster(*sans);
    GlyphRasterizer hebrew_raster(*hebrew);
    GlyphAtlas atlas({&sans_raster, &hebrew_raster});
    const FallbackShaper shaper({*sans, *hebrew});

    bool ok = expect(atlas.face_count() == 2, "two faces");

    // The same text drawn through the shaper's face ids: every glyph is placed and has ink
    // (the space has none), from the rasterizer of its own face.
    const std::string text = "a \xD7\xA9\xD7\x9C";
    const auto run = shaper.shape(text, tessera::ui::text::TextStyle{});
    ok = expect(run.glyphs.size() == 4 && run.missing_glyphs == 0, "four glyphs, none missing") &&
         ok;
    std::uint32_t hebrew_glyph = 0;
    for (const auto& glyph : run.glyphs) {
        const GlyphAtlasEntry& entry = atlas.get_glyph(glyph.id, 24, glyph.face);
        const bool space = glyph.cluster == 1;
        ok = expect(space || (entry.has_bitmap && has_ink(atlas, entry)),
                    "a covered glyph has a bitmap with ink") &&
             ok;
        ok = expect(entry.advance > 0.0f, "its advance comes from the face") && ok;
        if (glyph.face == 1) {
            hebrew_glyph = glyph.id;
        }
    }
    ok = expect(atlas.stats().misses == 4, "four glyphs rasterized once each") && ok;

    // The same glyph id on the other face is a different entry, not a hit.
    const std::uint64_t misses = atlas.stats().misses;
    const GlyphAtlasEntry& other = atlas.get_glyph(hebrew_glyph, 24, 0);
    ok = expect(atlas.stats().misses == misses + 1, "(face, glyph, size) is the key") && ok;
    const GlyphAtlasEntry* own = atlas.find_glyph(hebrew_glyph, 24, 1);
    ok = expect(own != nullptr &&
                    (own->x != other.x || own->y != other.y || own->page != other.page),
                "the two faces occupy different atlas rectangles") &&
         ok;
    (void)atlas.get_glyph(hebrew_glyph, 24, 1);
    ok = expect(atlas.stats().misses == misses + 1, "repeating a lookup hits") && ok;

    // Outside the stack: empty, counted, and nothing is cached.
    const std::uint64_t dropped = atlas.stats().dropped;
    const GlyphAtlasEntry& none = atlas.get_glyph(hebrew_glyph, 24, 7);
    ok = expect(!none.has_bitmap && none.advance == 0.0f && atlas.stats().dropped == dropped + 1,
                "an unknown face is an empty dropped entry") &&
         ok;
    ok = expect(atlas.find_glyph(hebrew_glyph, 24, 7) == nullptr, "and finds nothing") && ok;

    // The single-rasterizer constructor still works and uses face 0.
    GlyphAtlas single(sans_raster);
    ok = expect(single.face_count() == 1 && single.get('a', 16).has_bitmap, "single face atlas") &&
         ok;

    if (!ok) {
        return 1;
    }
    std::cout << "glyph_atlas_faces: ok\n";
    return 0;
}
