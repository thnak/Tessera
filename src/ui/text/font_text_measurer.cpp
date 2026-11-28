// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/weighted_shaper.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::text {
namespace {

// The wrap's advance seam. HarfBuzz reports the shaped advance of the run it is given; the wrap
// adds `word-spacing` per separator and applies `text-transform` itself, so this callback must
// return the bare advance of the bytes it receives (docs/design/text.md section 6.4).
float shaped_advance(std::string_view text, const TextStyle& style, const void* user) {
    return static_cast<const WeightedShaper*>(user)->shape(text, style).advance;
}

} // namespace

struct FontTextMeasurer::Impl {
    explicit Impl(std::vector<FontFace> faces)
        : shaper(faces) {
        for (FontFace& face : faces) {
            if (face.valid()) {
                metrics_providers.emplace_back(std::move(face));
            }
        }
    }

    // Member order matters: the providers hold another copy of the shared face state, and the
    // shaper borrows the font file bytes, so both outlive it. One provider per valid face, in the
    // shaper's face order, so `shaper.index_for(weight)` indexes both.
    WeightedShaper shaper;
    std::vector<FreeTypeFontMetricsProvider> metrics_providers;
};

FontTextMeasurer::FontTextMeasurer(FontFace face)
    : FontTextMeasurer(std::vector<FontFace>{std::move(face)}) {}

FontTextMeasurer::FontTextMeasurer(std::vector<FontFace> faces)
    : impl_(std::make_unique<Impl>(std::move(faces))) {}

FontTextMeasurer::~FontTextMeasurer() = default;
FontTextMeasurer::FontTextMeasurer(FontTextMeasurer&&) noexcept = default;
FontTextMeasurer& FontTextMeasurer::operator=(FontTextMeasurer&&) noexcept = default;

TextMetrics FontTextMeasurer::measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const {
    if (text.empty()) {
        return {};
    }
    if (impl_->metrics_providers.empty()) {
        return {};
    }
    const FontMetrics metrics =
        impl_->metrics_providers[impl_->shaper.index_for(style.font_weight)].metrics(style);
    const TextLayout layout =
        layout_text_measured(text, style, metrics, constraints, &shaped_advance, &impl_->shaper);
    return TextMetrics{layout.width, layout.height, layout.baseline,
                       static_cast<int>(layout.lines.size())};
}

} // namespace tessera::ui::text
