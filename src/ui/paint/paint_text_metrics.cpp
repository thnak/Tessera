// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::paint {
namespace {

float harfbuzz_advance(std::string_view text, const text::TextStyle& style, void* user) {
    return static_cast<text::HarfBuzzShaper*>(user)->shape(text, style).advance;
}

} // namespace

struct FontPaintTextMetrics::Impl {
    Impl(text::FontFace face, std::uint32_t source_id)
        : id(source_id),
          shaper(face),
          metrics_provider(std::move(face)) {}

    std::uint32_t id = 0;
    text::HarfBuzzShaper shaper;
    text::FreeTypeFontMetricsProvider metrics_provider;
};

FontPaintTextMetrics::FontPaintTextMetrics(text::FontFace face, std::uint32_t id)
    : impl_(std::make_unique<Impl>(std::move(face), id)) {}

FontPaintTextMetrics::~FontPaintTextMetrics() = default;

FontPaintTextMetrics::FontPaintTextMetrics(FontPaintTextMetrics&&) noexcept = default;
FontPaintTextMetrics& FontPaintTextMetrics::operator=(FontPaintTextMetrics&&) noexcept = default;

std::uint32_t FontPaintTextMetrics::id() const {
    return impl_->id;
}

text::FontMetrics FontPaintTextMetrics::metrics(const text::TextStyle& style) const {
    return impl_->metrics_provider.metrics(style);
}

float FontPaintTextMetrics::advance(std::string_view text, const text::TextStyle& style) const {
    // Raw HarfBuzz advance plus the `word-spacing` the wrap added per separator: the painted cursor
    // and the laid-out line use the same additive rule, so their sums agree.
    //
    // `text-transform` is applied before shaping so the advance is the width of the glyphs paint is
    // about to emit (uppercase advances can differ from lowercase in a real face). The mapping is
    // 1:1 in bytes, so callers' source offsets are unchanged.
    std::string transformed;
    std::string_view measured = text;
    if (style.text_transform != text::TextTransform::None) {
        transformed = text::apply_text_transform(text, style.text_transform);
        measured = transformed;
    }
    const float spacing = style.word_spacing > 0.0f ? style.word_spacing : 0.0f;
    return impl_->shaper.shape(measured, style).advance +
           spacing * static_cast<float>(text::word_separator_count(measured));
}

text::TextLayout FontPaintTextMetrics::layout(std::string_view text, const text::TextStyle& style,
                                              const text::TextConstraints& constraints) const {
    return text::layout_text_measured(text, style, metrics(style), constraints, &harfbuzz_advance,
                                      &impl_->shaper);
}

} // namespace tessera::ui::paint
