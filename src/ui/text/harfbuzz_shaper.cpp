// SPDX-License-Identifier: MIT

#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"

#include <utility>

#include <hb.h>

namespace tessera::ui::text {

struct HarfBuzzShaper::Impl {
    FontFace face;
    hb_blob_t* blob = nullptr;
    hb_face_t* hb_face = nullptr;
    hb_font_t* font = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (font != nullptr) {
            hb_font_destroy(font);
        }
        if (hb_face != nullptr) {
            hb_face_destroy(hb_face);
        }
        if (blob != nullptr) {
            hb_blob_destroy(blob);
        }
    }
};

HarfBuzzShaper::HarfBuzzShaper(FontFace face)
    : impl_(std::make_unique<Impl>()) {
    impl_->face = std::move(face);
    const std::span<const std::uint8_t> bytes = impl_->face.data();
    if (bytes.empty()) {
        impl_.reset();
        return;
    }

    // HarfBuzz reads the same bytes FreeType validated; the blob references the face's buffer,
    // which outlives the shaper.
    impl_->blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<unsigned int>(bytes.size()), HB_MEMORY_MODE_READONLY,
                                 nullptr, nullptr);
    impl_->hb_face = hb_face_create(impl_->blob, 0);
    impl_->font = hb_font_create(impl_->hb_face);

    // Positions come out in font units; callers scale them to pixels instead of mutating the
    // font's scale per style.
    const unsigned int upem = hb_face_get_upem(impl_->hb_face);
    hb_font_set_scale(impl_->font, static_cast<int>(upem), static_cast<int>(upem));
}

HarfBuzzShaper::~HarfBuzzShaper() = default;
HarfBuzzShaper::HarfBuzzShaper(HarfBuzzShaper&&) noexcept = default;
HarfBuzzShaper& HarfBuzzShaper::operator=(HarfBuzzShaper&&) noexcept = default;

ShapedRun HarfBuzzShaper::shape(std::string_view utf8, const TextStyle& style) const {
    ShapedRun run;
    if (impl_ == nullptr || impl_->font == nullptr || utf8.empty() || style.font_size <= 0.0f) {
        return run;
    }

    const float scale = style.font_size / static_cast<float>(hb_face_get_upem(impl_->hb_face));

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(impl_->font, buffer, nullptr, 0);

    unsigned int count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);

    run.right_to_left = hb_buffer_get_direction(buffer) == HB_DIRECTION_RTL;
    run.glyphs.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        Glyph glyph;
        glyph.id = info[i].codepoint;
        glyph.cluster = info[i].cluster;
        glyph.advance = static_cast<float>(positions[i].x_advance) * scale;
        glyph.x_offset = static_cast<float>(positions[i].x_offset) * scale;
        glyph.y_offset = static_cast<float>(positions[i].y_offset) * scale;
        run.advance += glyph.advance;
        run.glyphs.push_back(glyph);
    }

    const FontMetrics metrics = FreeTypeFontMetricsProvider(impl_->face).metrics(style);
    run.ascent = metrics.ascent;
    run.descent = metrics.descent;

    hb_buffer_destroy(buffer);
    return run;
}

} // namespace tessera::ui::text
