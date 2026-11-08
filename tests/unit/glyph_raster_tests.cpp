// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphRasterizer;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glyph_raster: " << message << "\n";
    }
    return condition;
}

bool same_bitmap(const GlyphBitmap& lhs, const GlyphBitmap& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.bearing_x == rhs.bearing_x &&
           lhs.bearing_y == rhs.bearing_y && lhs.advance == rhs.advance &&
           lhs.coverage == rhs.coverage;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "glyph_raster: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "glyph_raster: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    GlyphRasterizer rasterizer(*loaded);
    const GlyphBitmap a = rasterizer.rasterize(U'A', 16);
    if (!expect(a.width > 0 && a.height > 0, "A has outline pixels") ||
        !expect(a.advance > 0.0f, "A has advance") ||
        !expect(a.coverage.size() == static_cast<std::size_t>(a.width) * a.height,
                "coverage size") ||
        !expect(std::any_of(a.coverage.begin(), a.coverage.end(),
                            [](std::uint8_t value) { return value != 0; }),
                "coverage has ink")) {
        return 2;
    }

    const GlyphBitmap repeated = rasterizer.rasterize(U'A', 16);
    if (!expect(same_bitmap(a, repeated), "rasterization is deterministic")) {
        return 3;
    }

    const GlyphBitmap larger = rasterizer.rasterize(U'A', 32);
    if (!expect(larger.advance > a.advance * 1.5f, "advance scales with pixel size") ||
        !expect(larger.width > a.width, "bitmap grows with pixel size")) {
        return 4;
    }

    const GlyphBitmap space = rasterizer.rasterize(U' ', 16);
    if (!expect(space.width == 0 && space.height == 0, "space has no coverage") ||
        !expect(space.advance > 0.0f, "space keeps its advance")) {
        return 5;
    }

    const GlyphBitmap missing = rasterizer.rasterize(0x10FFFF, 16);
    if (!expect(missing.width > 0 && missing.height > 0, "missing codepoint renders .notdef")) {
        return 6;
    }

    GlyphRasterizer invalid{FontFace{}};
    const GlyphBitmap none = invalid.rasterize(U'A', 16);
    if (!expect(none.width == 0 && none.height == 0 && none.advance == 0.0f,
                "invalid face rasterizes nothing")) {
        return 7;
    }

    std::cout << "glyph_raster: ok\n";
    return 0;
}
