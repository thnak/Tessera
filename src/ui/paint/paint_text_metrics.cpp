// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/weighted_shaper.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::paint {
namespace {

float harfbuzz_advance(std::string_view text, const text::TextStyle& style, const void* user) {
    return static_cast<const text::WeightedShaper*>(user)->shape(text, style).advance;
}

} // namespace

struct FontPaintTextMetrics::Impl {
    Impl(std::vector<text::FontFace> faces, std::uint32_t source_id)
        : id(source_id),
          shaper(faces) {
        for (text::FontFace& face : faces) {
            if (face.valid()) {
                metrics_providers.emplace_back(std::move(face));
            }
        }
    }

    std::uint32_t id = 0;
    text::WeightedShaper shaper;
    // One per valid face in the shaper's order, so `shaper.index_for(weight)` indexes both.
    std::vector<text::FreeTypeFontMetricsProvider> metrics_providers;
};

FontPaintTextMetrics::FontPaintTextMetrics(text::FontFace face, std::uint32_t id)
    : FontPaintTextMetrics(std::vector<text::FontFace>{std::move(face)}, id) {}

FontPaintTextMetrics::FontPaintTextMetrics(std::vector<text::FontFace> faces, std::uint32_t id)
    : impl_(std::make_unique<Impl>(std::move(faces), id)) {}

FontPaintTextMetrics::~FontPaintTextMetrics() = default;

FontPaintTextMetrics::FontPaintTextMetrics(FontPaintTextMetrics&&) noexcept = default;
FontPaintTextMetrics& FontPaintTextMetrics::operator=(FontPaintTextMetrics&&) noexcept = default;

std::uint32_t FontPaintTextMetrics::id() const {
    return impl_->id;
}

text::FontMetrics FontPaintTextMetrics::metrics(const text::TextStyle& style) const {
    if (impl_->metrics_providers.empty()) {
        return {};
    }
    return impl_->metrics_providers[impl_->shaper.index_for(style.font_weight)].metrics(style);
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
