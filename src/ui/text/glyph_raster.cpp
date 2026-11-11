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

std::uint32_t GlyphRasterizer::glyph_index(char32_t codepoint) const {
    if (!face_.valid()) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        FT_Get_Char_Index(face_.state_->face, static_cast<FT_ULong>(codepoint)));
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, std::uint16_t pixel_size) const {
    return rasterize_glyph(glyph_index(codepoint), pixel_size);
}

GlyphBitmap GlyphRasterizer::rasterize_glyph(std::uint32_t glyph_index,
                                             std::uint16_t pixel_size) const {
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
    if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph_index), FT_LOAD_DEFAULT | FT_LOAD_COLOR) !=
        0) {
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
    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        // FreeType composites COLR layers into premultiplied BGRA; the atlas and the shader want
        // straight-alpha RGBA, so convert once here.
        result.width = bitmap.width;
        result.height = bitmap.rows;
        result.color = true;
        result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4);
        for (std::uint32_t row = 0; row < result.height; ++row) {
            const std::uint8_t* source =
                bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            std::uint8_t* destination =
                result.rgba.data() + static_cast<std::size_t>(row) * result.width * 4;
            for (std::uint32_t column = 0; column < result.width; ++column) {
                const std::uint32_t alpha = source[column * 4 + 3];
                const auto straight = [alpha](std::uint8_t value) {
                    return static_cast<std::uint8_t>(
                        alpha == 0
                            ? 0
                            : std::min<std::uint32_t>(255, (value * 255U + alpha / 2) / alpha));
                };
                destination[column * 4 + 0] = straight(source[column * 4 + 2]);
                destination[column * 4 + 1] = straight(source[column * 4 + 1]);
                destination[column * 4 + 2] = straight(source[column * 4 + 0]);
                destination[column * 4 + 3] = static_cast<std::uint8_t>(alpha);
            }
        }
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
