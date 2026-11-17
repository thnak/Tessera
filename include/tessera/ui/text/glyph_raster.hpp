// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"

#include <cstdint>
#include <vector>

namespace tessera::ui::text {

// How FreeType hints a glyph (display-aware-rendering.md section 3.2). `Font` is the font's own
// hints (FT_LOAD_DEFAULT, today's behavior and the golden default); `Light` is the vertical-only
// autohinter; `None` loads the unhinted outline.
enum class GlyphHinting : std::uint8_t { Font, Light, None };

// Horizontal glyph-origin fractions are quantized to quarter pixels (display-aware-rendering.md
// section 3.3): 0..3 is the fraction in units of 1/4 px. A larger value is clamped to 3.
inline constexpr std::uint8_t kSubpixelSteps = 4;

// An 8-bit coverage bitmap produced by FreeType. Glyphs without an outline (for example the
// space character) keep their advance but have no coverage pixels.
struct GlyphBitmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t bearing_x = 0;
    std::int32_t bearing_y = 0;
    float advance = 0.0f;
    // A color glyph (COLRv0 layers composited by FreeType, issue #12) carries straight-alpha RGBA8
    // in `rgba` (width x height x 4) and leaves `coverage` empty; every other glyph is coverage
    // only. Color glyphs scale with the pixel size like outlines; embedded bitmap strikes (CBDT,
    // sbix) need PNG support, which the pinned FreeType build disables.
    bool color = false;
    std::vector<std::uint8_t> coverage;
    std::vector<std::uint8_t> rgba;
};

// Rasterizes glyphs from one loaded face at integer pixel sizes. The rasterizer mutates the
// face's FreeType size/glyph slots, so it is not thread-safe even though queries are const.
class GlyphRasterizer {
public:
    explicit GlyphRasterizer(FontFace face);

    GlyphRasterizer(const GlyphRasterizer&) = delete;
    GlyphRasterizer& operator=(const GlyphRasterizer&) = delete;
    GlyphRasterizer(GlyphRasterizer&&) noexcept = default;
    GlyphRasterizer& operator=(GlyphRasterizer&&) noexcept = default;

    [[nodiscard]] std::uint32_t glyph_index(char32_t codepoint) const;
    [[nodiscard]] GlyphBitmap rasterize(char32_t codepoint, std::uint16_t pixel_size) const;
    // `x_subpixel` shifts the outline right by x_subpixel / kSubpixelSteps pixels before rendering,
    // so a glyph placed at a fractional pen position keeps its real shape instead of being
    // resampled.
    [[nodiscard]] GlyphBitmap rasterize_glyph(std::uint32_t glyph_index, std::uint16_t pixel_size,
                                              GlyphHinting hinting = GlyphHinting::Font,
                                              std::uint8_t x_subpixel = 0) const;

private:
    FontFace face_;
};

} // namespace tessera::ui::text
