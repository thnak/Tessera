// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::text {
namespace {

// The wrap's advance seam. HarfBuzz reports the shaped advance of the run it is given; the wrap
// adds `word-spacing` per separator and applies `text-transform` itself, so this callback must
// return the bare advance of the bytes it receives (docs/design/text.md section 6.4).
float shaped_advance(std::string_view text, const TextStyle& style, void* user) {
    return static_cast<const HarfBuzzShaper*>(user)->shape(text, style).advance;
}

} // namespace

struct FontTextMeasurer::Impl {
    explicit Impl(FontFace face)
        : shaper(face),
          metrics_provider(face) {}

    // Member order matters: `metrics_provider` holds another copy of the shared face state, and the
    // shaper borrows the font file bytes, so both outlive it. The face itself is not stored: the
    // shaper and the provider each keep what they need.
    HarfBuzzShaper shaper;
    FreeTypeFontMetricsProvider metrics_provider;
};

FontTextMeasurer::FontTextMeasurer(FontFace face)
    : impl_(std::make_unique<Impl>(std::move(face))) {}

FontTextMeasurer::~FontTextMeasurer() = default;
FontTextMeasurer::FontTextMeasurer(FontTextMeasurer&&) noexcept = default;
FontTextMeasurer& FontTextMeasurer::operator=(FontTextMeasurer&&) noexcept = default;

TextMetrics FontTextMeasurer::measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const {
    if (text.empty()) {
        return {};
    }
    const FontMetrics metrics = impl_->metrics_provider.metrics(style);
    const TextLayout layout =
        layout_text_measured(text, style, metrics, constraints, &shaped_advance, &impl_->shaper);
    return TextMetrics{layout.width, layout.height, layout.baseline,
                       static_cast<int>(layout.lines.size())};
}

} // namespace tessera::ui::text
