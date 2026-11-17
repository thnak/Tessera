// SPDX-License-Identifier: MIT

// LCD glyph rasterization (display-aware-rendering.md section 3.4, slice 2 of #471): FreeType's
// FT_RENDER_MODE_LCD produces three coverage samples per pixel, so the bitmap is three times as
// wide and its channels differ across a stem; the result carries the mode it was rasterized in; and
// a glyph FreeType cannot render as LCD (no outline: a color glyph, a bitmap glyph, whitespace)
// falls back to the grayscale bitmap instead of losing the glyph. The default stays grayscale, so
// the golden path is byte-identical. argv[1] is NotoSans, argv[2] is ColorTest.ttf.

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::coverage_channels;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAaMode;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphHinting;
using tessera::ui::text::GlyphRasterizer;

constexpr std::uint16_t kPixelSize = 16;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_lcd_raster: " << message << "\n";
    }
    return condition;
}

bool same_bitmap(const GlyphBitmap& a, const GlyphBitmap& b) {
    return a.width == b.width && a.height == b.height && a.bearing_x == b.bearing_x &&
           a.bearing_y == b.bearing_y && a.advance == b.advance && a.color == b.color &&
           a.aa_mode == b.aa_mode && a.coverage == b.coverage && a.rgba == b.rgba;
}

// The largest R/G/B spread over one pixel: the direct evidence that the three subpixel coverages
// are genuinely different samples and not one scalar replicated three times.
int max_channel_spread(const GlyphBitmap& bitmap) {
    int spread = 0;
    for (std::uint32_t y = 0; y < bitmap.height; ++y) {
        for (std::uint32_t x = 0; x < bitmap.width / 3; ++x) {
            const std::uint8_t* pixel =
                &bitmap.coverage[(static_cast<std::size_t>(y) * bitmap.width) + x * 3];
            const int low = std::min({pixel[0], pixel[1], pixel[2]});
            const int high = std::max({pixel[0], pixel[1], pixel[2]});
            spread = std::max(spread, high - low);
        }
    }
    return spread;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "text_lcd_raster: need the NotoSans and ColorTest paths\n";
        return 1;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto color = FontFace::load_from_file(argv[2]);
    if (!sans || !color) {
        std::cerr << "text_lcd_raster: cannot load the fixture fonts\n";
        return 1;
    }
    GlyphRasterizer rasterizer(*sans);
    GlyphRasterizer color_rasterizer(*color);
    bool ok = true;

    // (a) The default is grayscale, and asking for it explicitly is the same bitmap: the golden
    // path cannot move because the parameter exists.
    const std::uint32_t h = rasterizer.glyph_index(U'H');
    const GlyphBitmap gray = rasterizer.rasterize_glyph(h, kPixelSize);
    ok &= expect(gray.aa_mode == GlyphAaMode::Grayscale, "the default mode is grayscale");
    ok &= expect(same_bitmap(gray, rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::Font, 0,
                                                              GlyphAaMode::Grayscale)),
                 "the explicit grayscale mode is the default bitmap");
    ok &= expect(gray.coverage.size() == static_cast<std::size_t>(gray.width) * gray.height,
                 "grayscale coverage is one sample per pixel");

    // (b) LCD is three samples per pixel: the bitmap is three times as wide, the height and the
    // bearings are the glyph's own, and the coverage buffer is three times as large.
    const GlyphBitmap lcd =
        rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::Font, 0, GlyphAaMode::Lcd);
    ok &= expect(lcd.aa_mode == GlyphAaMode::Lcd, "the LCD bitmap reports its mode");
    ok &= expect(lcd.width % 3 == 0, "an LCD bitmap's width is a whole number of pixels");
    // FreeType pads the LCD box by the subpixel geometry (about a third of a pixel each side), so
    // the footprint is the grayscale width or up to two pixels wider - never narrower, and never
    // the grayscale width itself (that would mean the three channels were not separated).
    ok &= expect(lcd.width / 3 >= gray.width && lcd.width / 3 <= gray.width + 2,
                 "LCD triples the width of the glyph's pixel footprint");
    ok &= expect(lcd.height == gray.height, "LCD keeps the glyph's height");
    // FreeType pads the LCD box by the subpixel geometry, which can move `bitmap_left` by one pixel
    // (the leftmost subpixel sample sits a third of a pixel to the left). The bearing therefore
    // tracks the padded box, and the quad placement uses it, so the glyph still lands where the
    // grayscale one does.
    ok &= expect(std::abs(lcd.bearing_x - gray.bearing_x) <= 1,
                 "LCD keeps the glyph's horizontal bearing within the subpixel padding");
    ok &= expect(lcd.bearing_y == gray.bearing_y, "LCD keeps the glyph's vertical bearing");
    ok &= expect(lcd.advance == gray.advance, "LCD keeps the glyph's advance");
    ok &= expect(lcd.coverage.size() == static_cast<std::size_t>(lcd.width) * lcd.height,
                 "LCD coverage is one sample per subpixel");
    ok &= expect(coverage_channels(GlyphAaMode::Lcd) == 3 &&
                     coverage_channels(GlyphAaMode::Grayscale) == 1,
                 "the channel counts are 3 and 1");

    // (c) The three channels are different samples, not one scalar replicated: a stem's edge shows
    // a large R/G/B spread. This is the property the dual-source blend exists to composite.
    ok &= expect(max_channel_spread(lcd) > 32,
                 "a stem's R/G/B coverages differ (the subpixels are distinct samples)");
    // The same glyph rasterized twice is the same bitmap: LCD is deterministic.
    ok &= expect(same_bitmap(lcd, rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::Font, 0,
                                                             GlyphAaMode::Lcd)),
                 "LCD rasterization is deterministic");

    // (d) A glyph with no outline falls back to grayscale rather than failing. Whitespace has no
    // coverage at all in either mode, and the mode says grayscale because that is what was
    // produced.
    const GlyphBitmap space = rasterizer.rasterize_glyph(rasterizer.glyph_index(U' '), kPixelSize,
                                                         GlyphHinting::Font, 0, GlyphAaMode::Lcd);
    ok &= expect(space.width == 0 && space.height == 0, "whitespace has no coverage in LCD mode");
    ok &= expect(space.aa_mode == GlyphAaMode::Grayscale,
                 "whitespace falls back to the grayscale mode");
    ok &= expect(space.advance > 0.0f, "whitespace keeps its advance in LCD mode");

    // A color glyph is a bitmap, not an outline: FreeType's LCD renderer refuses it, so the
    // rasterizer must fall back to the color bitmap instead of returning nothing.
    const GlyphBitmap emoji = color_rasterizer.rasterize_glyph(
        color_rasterizer.glyph_index(0x1F600), 64, GlyphHinting::Font, 0, GlyphAaMode::Lcd);
    ok &= expect(emoji.color && emoji.width > 0 && emoji.height > 0,
                 "a color glyph still rasterizes in LCD mode");
    ok &= expect(emoji.aa_mode == GlyphAaMode::Grayscale,
                 "a color glyph falls back to the grayscale mode");
    ok &= expect(emoji.coverage.empty() &&
                     emoji.rgba.size() == static_cast<std::size_t>(emoji.width) * emoji.height * 4,
                 "the color fallback keeps the RGBA layout");
    // The fallback is the same bitmap the grayscale request produces, not a degraded one.
    ok &= expect(same_bitmap(emoji, color_rasterizer.rasterize_glyph(
                                        color_rasterizer.glyph_index(0x1F600), 64)),
                 "the color fallback is the grayscale bitmap");

    // (e) LCD composes with the other rasterizer parameters: hinting and a quarter-pixel origin
    // still apply, and the result stays three samples per pixel.
    const GlyphBitmap unhinted =
        rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::None, 0, GlyphAaMode::Lcd);
    ok &= expect(unhinted.aa_mode == GlyphAaMode::Lcd && unhinted.width % 3 == 0,
                 "LCD works with unhinted outlines");
    const GlyphBitmap shifted =
        rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::None, 2, GlyphAaMode::Lcd);
    ok &= expect(shifted.aa_mode == GlyphAaMode::Lcd && shifted.coverage != unhinted.coverage,
                 "a quarter-pixel origin still changes LCD coverage");

    if (!ok) {
        return 1;
    }
    std::cout << "text_lcd_raster: ok\n";
    return 0;
}
