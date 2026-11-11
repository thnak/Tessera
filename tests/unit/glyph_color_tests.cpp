// SPDX-License-Identifier: MIT
//
// Color glyphs (issue #12 slice c): a COLRv0 face rasterizes to straight-alpha RGBA, plain faces
// stay coverage only, the atlas stores each kind in the same RGBA8 pages and flags color entries,
// and a font stack picks the color face for emoji. argv[1] is NotoSans, argv[2] is
// ColorTest.ttf (tests/fixtures/fonts/make_color_test_font.py).

#include "tessera/ui/text/fallback_shaper.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glyph_color: " << message << "\n";
    }
    return condition;
}

using Rgba = std::array<int, 4>;

Rgba at(const GlyphBitmap& bitmap, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
    return {bitmap.rgba[i], bitmap.rgba[i + 1], bitmap.rgba[i + 2], bitmap.rgba[i + 3]};
}

bool near(const Rgba& got, const Rgba& want, int tolerance = 6) {
    for (int i = 0; i < 4; ++i) {
        if (std::abs(got[i] - want[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

// The pixel under the glyph-space point (fx, fy) in em units, y up from the baseline.
Rgba sample(const GlyphBitmap& bitmap, std::uint16_t pixel_size, float fx, float fy) {
    const int x = static_cast<int>(fx * pixel_size) - bitmap.bearing_x;
    const int y = bitmap.bearing_y - static_cast<int>(fy * pixel_size);
    if (x < 0 || y < 0 || x >= static_cast<int>(bitmap.width) ||
        y >= static_cast<int>(bitmap.height)) {
        return {0, 0, 0, 0};
    }
    return at(bitmap, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
}

bool rasterizes_color(const GlyphRasterizer& color) {
    constexpr std::uint16_t kSize = 64;
    const GlyphBitmap face = color.rasterize(0x1F600, kSize);
    bool ok = expect(face.color && face.width > 0 && face.height > 0 && face.coverage.empty() &&
                         face.rgba.size() == static_cast<std::size_t>(face.width) * face.height * 4,
                     "the face is a color bitmap");
    if (!ok) {
        return false;
    }
    // Disc center is yellow (palette 0), an eye is black (palette 1), outside the disc is clear.
    ok = expect(near(sample(face, kSize, 0.50f, 0.20f), {255, 217, 0, 255}), "yellow disc") && ok;
    ok = expect(near(sample(face, kSize, 0.35f, 0.50f), {0, 0, 0, 255}), "black eye on top") && ok;
    ok = expect(near(sample(face, kSize, 0.65f, 0.50f), {0, 0, 0, 255}), "second eye") && ok;
    ok =
        expect(sample(face, kSize, 0.05f, 0.02f)[3] == 0, "corner outside the disc is clear") && ok;
    ok = expect(face.advance > 0.0f, "advance") && ok;

    const GlyphBitmap box = color.rasterize(0x1F601, kSize);
    ok = expect(box.color && near(sample(box, kSize, 0.50f, 0.40f), {230, 26, 26, 255}),
                "single layer box is red") &&
         ok;

    // Scalable: a bigger size is a bigger bitmap.
    const GlyphBitmap big = color.rasterize(0x1F600, 128);
    ok = expect(big.color && big.width > face.width, "scales with the pixel size") && ok;
    return ok;
}

bool plain_stays_coverage(const GlyphRasterizer& plain) {
    const GlyphBitmap a = plain.rasterize('a', 32);
    return expect(!a.color && a.rgba.empty() && !a.coverage.empty(), "a plain glyph is coverage");
}

bool atlas_and_stack(const FontFace& sans, const FontFace& color_face, GlyphRasterizer& sans_raster,
                     GlyphRasterizer& color_raster) {
    const FallbackShaper shaper({sans, color_face});
    // "a", the face, "b": the emoji exists only in the second face.
    const auto run = shaper.shape("a\xF0\x9F\x98\x80"
                                  "b",
                                  TextStyle{});
    bool ok =
        expect(run.glyphs.size() == 3 && run.missing_glyphs == 0, "three glyphs, none missing");
    if (!ok) {
        return false;
    }
    ok = expect(run.glyphs[0].face == 0 && run.glyphs[1].face == 1 && run.glyphs[2].face == 0,
                "the emoji falls through to the color face") &&
         ok;

    GlyphAtlas atlas({&sans_raster, &color_raster});
    const GlyphAtlasEntry& letter = atlas.get_glyph(run.glyphs[0].id, 32, run.glyphs[0].face);
    const GlyphAtlasEntry letter_copy = letter;
    const GlyphAtlasEntry& emoji = atlas.get_glyph(run.glyphs[1].id, 32, run.glyphs[1].face);
    ok = expect(!letter_copy.color && letter_copy.has_bitmap, "letter entry is coverage") && ok;
    ok = expect(emoji.color && emoji.has_bitmap, "emoji entry is flagged color") && ok;

    // The page holds the emoji's own colors, not white: some opaque non-white pixel is present.
    const auto pixels = atlas.page_pixels(emoji.page);
    bool saw_yellow = false;
    bool saw_black = false;
    for (std::uint32_t y = 0; y < emoji.height; ++y) {
        for (std::uint32_t x = 0; x < emoji.width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(emoji.y + y) * atlas.page_width() + emoji.x + x) * 4;
            saw_yellow = saw_yellow || (pixels[i] > 240 && pixels[i + 1] > 200 &&
                                        pixels[i + 2] < 20 && pixels[i + 3] == 255);
            saw_black = saw_black || (pixels[i] < 10 && pixels[i + 1] < 10 && pixels[i + 2] < 10 &&
                                      pixels[i + 3] == 255);
        }
    }
    ok = expect(saw_yellow && saw_black, "the page stores the glyph's colors") && ok;
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "glyph_color: need the NotoSans and ColorTest paths\n";
        return 64;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto color = FontFace::load_from_file(argv[2]);
    if (!sans || !color) {
        std::cerr << "glyph_color: cannot load the fonts\n";
        return 65;
    }
    GlyphRasterizer sans_raster(*sans);
    GlyphRasterizer color_raster(*color);

    bool ok = rasterizes_color(color_raster);
    ok = plain_stays_coverage(sans_raster) && ok;
    ok = atlas_and_stack(*sans, *color, sans_raster, color_raster) && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "glyph_color: ok\n";
    return 0;
}
