// SPDX-License-Identifier: MIT
//
// A character no face covers must stay visible (docs/design/text.md, issue #12). Faces with a drawn
// .notdef show it; a face whose .notdef is empty gets a deterministic hollow box instead, and a
// real empty glyph (a space) stays empty. argv[1] is NotoSans, argv[2] is EmptyNotdef.ttf
// (ColorTest.ttf with the .notdef outline removed).

#include "tessera/ui/text/fallback_shaper.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "tofu_placeholder: " << message << "\n";
    }
    return condition;
}

std::size_t ink(const GlyphBitmap& bitmap) {
    std::size_t count = 0;
    for (const std::uint8_t value : bitmap.coverage) {
        count += value > 127 ? 1U : 0U;
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: tofu_placeholder_tests <NotoSans.ttf> <EmptyNotdef.ttf>\n";
        return 2;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto empty = FontFace::load_from_file(argv[2]);
    if (!sans || !empty) {
        std::cerr << "tofu_placeholder: fixture fonts failed to load\n";
        return 2;
    }

    const GlyphRasterizer sans_raster(*sans);
    const GlyphRasterizer empty_raster(*empty);
    const GlyphBitmap drawn = sans_raster.rasterize_glyph(0, 24);
    const GlyphBitmap box = empty_raster.rasterize_glyph(0, 24);
    const GlyphBitmap again = empty_raster.rasterize_glyph(0, 24);

    bool ok = expect(ink(drawn) > 0, "a face with a drawn .notdef keeps its own tofu");
    ok = expect(!box.color && box.width >= 3 && box.height >= 3 && ink(box) > 0,
                "an empty .notdef gets a visible placeholder") &&
         ok;
    ok = expect(box.advance > 0.0f && box.bearing_y == static_cast<std::int32_t>(box.height),
                "the placeholder has an advance and sits on the baseline") &&
         ok;
    ok = expect(box.coverage == again.coverage && box.width == again.width,
                "the placeholder is deterministic") &&
         ok;
    // Hollow: the corner is inked and the middle pixel is empty.
    ok = expect(box.coverage[0] == 255 &&
                    box.coverage[(box.height / 2) * box.width + box.width / 2] == 0,
                "the placeholder is a hollow box") &&
         ok;
    const GlyphBitmap small = empty_raster.rasterize_glyph(0, 8);
    ok = expect(small.width >= 3 && ink(small) > 0, "it stays visible at small sizes") && ok;

    // A real glyph that is empty by design must not turn into tofu.
    const GlyphBitmap space = sans_raster.rasterize(U' ', 24);
    ok = expect(space.width == 0 && space.advance > 0.0f, "a space stays empty") && ok;

    // Through the shaper: an uncovered cluster is counted and rasterizes to ink.
    std::vector<FontFace> stack;
    stack.push_back(*empty);
    const FallbackShaper shaper(std::move(stack));
    const auto run = shaper.shape("\xE0\xB8\x81", TextStyle{}); // Thai KO KAI, not in the fixture
    ok = expect(run.missing_glyphs == 1 && run.glyphs.size() == 1 && run.glyphs[0].id == 0,
                "an uncovered character shapes to one counted .notdef") &&
         ok;
    ok = expect(ink(empty_raster.rasterize_glyph(run.glyphs[0].id, 24)) > 0,
                "that glyph draws the placeholder") &&
         ok;

    if (!ok) {
        return 1;
    }
    std::cout << "tofu_placeholder: ok\n";
    return 0;
}
