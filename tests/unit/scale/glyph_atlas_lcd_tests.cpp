// SPDX-License-Identifier: MIT

// The AA mode is atlas identity (display-aware-rendering.md section 3.4, slice 2 of #471): one
// atlas is one (mode, hinting) pair, so two modes are two atlases with their own pages and bitmaps;
// an entry exposes the channels its page holds, including the per-glyph grayscale fallback inside
// an LCD atlas; the LCD upload writes per-subpixel coverage into RGB (with the per-pixel maximum in
// alpha) instead of the grayscale alpha-only path; and ScaleGlyphAtlas::set_aa_mode releases the
// whole set exactly like set_hinting. argv[1] is NotoSans, argv[2] is ColorTest.ttf.

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAaMode;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasConfig;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphHinting;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::ScaleGlyphAtlas;

constexpr std::uint16_t kPixelSize = 16;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glyph_atlas_lcd: " << message << "\n";
    }
    return condition;
}

GlyphAtlasConfig config_for(GlyphAaMode mode) {
    GlyphAtlasConfig config;
    config.page_width = 128;
    config.page_height = 128;
    config.max_pages = 2;
    config.padding = 1;
    config.aa_mode = mode;
    return config;
}

// The page texel under (column, row) of an entry, as four bytes.
struct Texel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

Texel texel_at(const GlyphAtlas& atlas, const GlyphAtlasEntry& entry, std::uint32_t column,
               std::uint32_t row) {
    const auto pixels = atlas.page_pixels(entry.page);
    const std::size_t index =
        ((static_cast<std::size_t>(entry.y + row) * atlas.page_width()) + entry.x + column) * 4;
    if (index + 3 >= pixels.size()) {
        return {};
    }
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "glyph_atlas_lcd: need the NotoSans and ColorTest paths\n";
        return 1;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto color = FontFace::load_from_file(argv[2]);
    if (!sans || !color) {
        std::cerr << "glyph_atlas_lcd: cannot load the fixture fonts\n";
        return 1;
    }
    GlyphRasterizer rasterizer(*sans);
    GlyphRasterizer color_rasterizer(*color);
    bool ok = true;

    // (a) The mode is part of the atlas identity: two modes are two atlases, each with its own
    // pages, and the same glyph is two entries whose bitmaps differ.
    GlyphAtlas gray_atlas(rasterizer, config_for(GlyphAaMode::Grayscale));
    GlyphAtlas lcd_atlas(rasterizer, config_for(GlyphAaMode::Lcd));
    gray_atlas.begin_frame();
    lcd_atlas.begin_frame();
    ok &= expect(gray_atlas.aa_mode() == GlyphAaMode::Grayscale &&
                     lcd_atlas.aa_mode() == GlyphAaMode::Lcd,
                 "each atlas reports its own mode");

    const std::uint32_t h = rasterizer.glyph_index(U'H');
    const GlyphAtlasEntry gray_entry = gray_atlas.get_glyph(h, kPixelSize);
    const GlyphAtlasEntry lcd_entry = lcd_atlas.get_glyph(h, kPixelSize);
    ok &= expect(gray_entry.has_bitmap && lcd_entry.has_bitmap, "both modes place the glyph");
    ok &= expect(gray_entry.channels == 1, "a grayscale entry has one channel");
    ok &= expect(lcd_entry.channels == 3, "an LCD entry has three channels");
    ok &= expect(lcd_entry.width == gray_entry.width * 3 || lcd_entry.width / 3 >= gray_entry.width,
                 "the LCD entry's page extent is three samples per pixel");
    ok &= expect(lcd_entry.width % 3 == 0, "the LCD entry's width is a whole pixel count");
    ok &= expect(lcd_entry.height == gray_entry.height, "both modes keep the glyph's height");
    ok &= expect(lcd_entry.advance == gray_entry.advance, "both modes keep the glyph's advance");
    // Two atlases, so the same glyph occupies its own page in each: the entries are independent.
    ok &= expect(gray_atlas.page_count() == 1 && lcd_atlas.page_count() == 1,
                 "each mode owns its page");
    ok &= expect(gray_atlas.page_pixels(0).size() == lcd_atlas.page_pixels(0).size(),
                 "both modes use the same RGBA8 page format");

    // (b) The uploads differ in the way the modes differ: grayscale writes coverage into alpha and
    // leaves RGB white; LCD writes per-subpixel coverage into RGB and the per-pixel maximum into
    // alpha, so the same page still renders through the grayscale pipeline.
    bool gray_alpha_ink = false;
    bool gray_rgb_white = true;
    for (std::uint32_t row = 0; row < gray_entry.height; ++row) {
        for (std::uint32_t column = 0; column < gray_entry.width; ++column) {
            const Texel texel = texel_at(gray_atlas, gray_entry, column, row);
            gray_alpha_ink = gray_alpha_ink || texel.a != 0;
            gray_rgb_white = gray_rgb_white && texel.r == 255 && texel.g == 255 && texel.b == 255;
        }
    }
    ok &= expect(gray_alpha_ink, "the grayscale page carries coverage in alpha");
    ok &= expect(gray_rgb_white, "the grayscale page keeps white RGB");

    bool lcd_channel_ink = false;
    bool lcd_channels_differ = false;
    bool lcd_alpha_is_max = true;
    for (std::uint32_t row = 0; row < lcd_entry.height; ++row) {
        for (std::uint32_t column = 0; column < lcd_entry.width; ++column) {
            const Texel texel = texel_at(lcd_atlas, lcd_entry, column, row);
            lcd_channel_ink = lcd_channel_ink || texel.r != 0 || texel.g != 0 || texel.b != 0;
            lcd_channels_differ = lcd_channels_differ || texel.r != texel.g || texel.g != texel.b;
            lcd_alpha_is_max = lcd_alpha_is_max && texel.a == std::max({texel.r, texel.g, texel.b});
        }
    }
    ok &= expect(lcd_channel_ink, "the LCD page carries coverage in RGB");
    ok &= expect(lcd_channels_differ, "the LCD page's R/G/B are distinct subpixel coverages");
    ok &= expect(lcd_alpha_is_max,
                 "the LCD page's alpha is the per-pixel maximum (the grayscale fallback)");

    // The page holds exactly the rasterizer's coverage. FreeType's LCD row is one RGB triple per
    // pixel while the page rect is three texels per pixel, so each pixel's three texels must carry
    // that pixel's whole triple; reading the interleaved row with the texel index instead of the
    // pixel index is the easy mistake here, and only an exact comparison catches it.
    const GlyphBitmap lcd_bitmap =
        rasterizer.rasterize_glyph(h, kPixelSize, GlyphHinting::Font, 0, GlyphAaMode::Lcd);
    bool page_matches_bitmap = lcd_bitmap.aa_mode == GlyphAaMode::Lcd &&
                               lcd_bitmap.width == lcd_entry.width &&
                               lcd_bitmap.height == lcd_entry.height;
    for (std::uint32_t row = 0; row < lcd_entry.height && page_matches_bitmap; ++row) {
        for (std::uint32_t column = 0; column < lcd_entry.width; ++column) {
            const std::uint8_t* pixel =
                &lcd_bitmap.coverage[(static_cast<std::size_t>(row) * lcd_bitmap.width) +
                                     (column / 3) * 3];
            const Texel texel = texel_at(lcd_atlas, lcd_entry, column, row);
            if (texel.r != pixel[0] || texel.g != pixel[1] || texel.b != pixel[2] ||
                texel.a != std::max({pixel[0], pixel[1], pixel[2]})) {
                page_matches_bitmap = false;
                break;
            }
        }
    }
    ok &= expect(page_matches_bitmap, "the LCD page holds the rasterizer's coverage exactly");
    if (lcd_entry.width >= 3 && lcd_entry.height > 0) {
        const Texel first = texel_at(lcd_atlas, lcd_entry, 0, 0);
        const Texel second = texel_at(lcd_atlas, lcd_entry, 1, 0);
        const Texel third = texel_at(lcd_atlas, lcd_entry, 2, 0);
        ok &= expect(first.r == second.r && second.r == third.r && first.g == second.g &&
                         second.g == third.g && first.b == second.b && second.b == third.b,
                     "a pixel's three sample columns carry the same triple");
    }
    // The cleared part of an LCD page is zero coverage, not the grayscale white: a white background
    // would bleed full coverage into a glyph edge through the linear filter.
    const auto lcd_pixels = lcd_atlas.page_pixels(0);
    const std::size_t clear_index =
        (static_cast<std::size_t>(lcd_atlas.page_height() - 1) * lcd_atlas.page_width() +
         lcd_atlas.page_width() - 1) *
        4;
    ok &= expect(lcd_pixels[clear_index] == 0 && lcd_pixels[clear_index + 1] == 0 &&
                     lcd_pixels[clear_index + 2] == 0,
                 "an LCD page's cleared texels are zero coverage");

    // (c) A glyph FreeType cannot render as LCD keeps its grayscale bitmap inside the LCD atlas,
    // and the entry says so: `channels` is per glyph, not per atlas, so a renderer's quad width is
    // right for both kinds in one page.
    GlyphAtlas mixed(rasterizer, config_for(GlyphAaMode::Lcd));
    mixed.begin_frame();
    const GlyphAtlasEntry space = mixed.get_glyph(rasterizer.glyph_index(U' '), kPixelSize);
    ok &= expect(!space.has_bitmap, "whitespace has no bitmap in an LCD atlas");
    ok &= expect(space.channels == 1, "a glyph with no outline reports one channel");
    ok &= expect(space.advance > 0.0f, "whitespace keeps its advance in an LCD atlas");

    GlyphAtlas color_atlas(color_rasterizer, config_for(GlyphAaMode::Lcd));
    color_atlas.begin_frame();
    const GlyphAtlasEntry emoji = color_atlas.get_glyph(color_rasterizer.glyph_index(0x1F600), 64);
    ok &= expect(emoji.has_bitmap && emoji.color, "a color glyph is placed in an LCD atlas");
    ok &= expect(emoji.channels == 1, "a color glyph falls back to one channel");
    ok &= expect(emoji.width % 3 != 0 || emoji.width == 0,
                 "the color fallback keeps its own width, not a tripled one");

    // (d) ScaleGlyphAtlas::set_aa_mode mirrors set_hinting: the value in force is a no-op, a change
    // releases every atlas (their bitmaps are stale), and the new mode is stored.
    ScaleGlyphAtlas atlases(rasterizer, config_for(GlyphAaMode::Grayscale));
    ok &= expect(atlases.aa_mode() == GlyphAaMode::Grayscale, "a fresh set is grayscale");
    (void)atlases.atlas_for(1.0f);
    (void)atlases.atlas_for(2.0f);
    ok &= expect(atlases.atlas_count() == 2, "two steps are live");
    atlases.set_aa_mode(GlyphAaMode::Grayscale);
    ok &= expect(atlases.atlas_count() == 2, "the mode already in force is a no-op");
    const std::uint64_t invalidated_before = atlases.invalidated_count();
    atlases.set_aa_mode(GlyphAaMode::Lcd);
    ok &= expect(atlases.aa_mode() == GlyphAaMode::Lcd, "the new mode is stored");
    ok &= expect(atlases.atlas_count() == 0, "an AA-mode change releases every atlas");
    ok &= expect(atlases.invalidated_count() == invalidated_before + 2,
                 "the release is counted per atlas");
    // The rebuilt atlas is an LCD one: the mode reached the config the new atlas was built from.
    GlyphAtlas& rebuilt = atlases.atlas_for(1.0f);
    ok &= expect(rebuilt.aa_mode() == GlyphAaMode::Lcd, "the rebuilt atlas uses the new mode");
    ok &= expect(rebuilt.get_glyph(h, kPixelSize).channels == 3,
                 "the rebuilt atlas rasterizes LCD coverage");

    // (e) The mode and the hinting are independent axes of the same identity: changing one leaves
    // the other in force, and each change releases the set.
    atlases.set_hinting(GlyphHinting::None);
    ok &= expect(atlases.aa_mode() == GlyphAaMode::Lcd, "a hinting change keeps the AA mode");
    ok &= expect(atlases.atlas_count() == 0, "a hinting change still releases the set");
    atlases.set_aa_mode(GlyphAaMode::Grayscale);
    ok &= expect(atlases.hinting() == GlyphHinting::None, "an AA-mode change keeps the hinting");

    if (!ok) {
        return 1;
    }
    std::cout << "glyph_atlas_lcd: ok\n";
    return 0;
}
