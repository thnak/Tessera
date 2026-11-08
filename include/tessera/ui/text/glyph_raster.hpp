// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"

#include <cstdint>
#include <vector>

namespace tessera::ui::text {

// An 8-bit coverage bitmap produced by FreeType. Glyphs without an outline (for example the
// space character) keep their advance but have no coverage pixels.
struct GlyphBitmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t bearing_x = 0;
    std::int32_t bearing_y = 0;
    float advance = 0.0f;
    std::vector<std::uint8_t> coverage;
};

// Rasterizes codepoints from one loaded face at integer pixel sizes. The rasterizer mutates the
// face's FreeType size/glyph slots, so it is not thread-safe and not const.
class GlyphRasterizer {
public:
    explicit GlyphRasterizer(FontFace face);

    GlyphRasterizer(const GlyphRasterizer&) = delete;
    GlyphRasterizer& operator=(const GlyphRasterizer&) = delete;
    GlyphRasterizer(GlyphRasterizer&&) noexcept = default;
    GlyphRasterizer& operator=(GlyphRasterizer&&) noexcept = default;

    [[nodiscard]] GlyphBitmap rasterize(char32_t codepoint, std::uint16_t pixel_size);

private:
    FontFace face_;
};

} // namespace tessera::ui::text
