// SPDX-License-Identifier: MIT

#include "font_face_state.hpp"
#include "tessera/ui/text/glyph_raster.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tessera::ui::text {

GlyphRasterizer::GlyphRasterizer(FontFace face)
    : face_(std::move(face)) {}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, std::uint16_t pixel_size) {
    GlyphBitmap result;
    if (!face_.valid() || pixel_size == 0) {
        return result;
    }

    FT_Face face = face_.state_->face;
    if (FT_Set_Pixel_Sizes(face, 0, pixel_size) != 0) {
        return result;
    }

    // A missing codepoint maps to glyph 0 (.notdef), which keeps fallback visible instead of
    // silently dropping the character.
    const FT_UInt glyph_index = FT_Get_Char_Index(face, static_cast<FT_ULong>(codepoint));
    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) != 0) {
        return result;
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return result;
    }

    const FT_GlyphSlot slot = face->glyph;
    result.advance = static_cast<float>(slot->advance.x) / 64.0f;
    result.bearing_x = slot->bitmap_left;
    result.bearing_y = slot->bitmap_top;

    const FT_Bitmap& bitmap = slot->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0) {
        return result;
    }
    // The atlas stores 8-bit coverage; hinted grayscale is the deterministic default.
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        return result;
    }

    result.width = bitmap.width;
    result.height = bitmap.rows;
    result.coverage.resize(static_cast<std::size_t>(result.width) * result.height);
    for (std::uint32_t row = 0; row < result.height; ++row) {
        std::memcpy(result.coverage.data() + static_cast<std::size_t>(row) * result.width,
                    bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch, result.width);
    }
    return result;
}

} // namespace tessera::ui::text
