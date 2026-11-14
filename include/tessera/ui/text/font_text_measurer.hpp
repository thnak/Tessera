// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <memory>

namespace tessera::ui::text {

// The real-face `TextMeasurer`: layout sizes a text box from the same FreeType line metrics and
// HarfBuzz shaping advances the glyph renderer draws with, so a content-sized box agrees with its
// painted ink (issue #314).
//
// `StubTextMeasurer` is a fixed-advance approximation (`0.5 * font_size` per code point) that has
// nothing to do with the loaded face. It stays the default so unit tests and font-free paths keep
// their deterministic boxes, but any host that both lays out and paints real text must install this
// measurer instead: mixing the stub layout width with HarfBuzz glyphs makes an auto-width box
// narrower or wider than its ink by a string-dependent amount (the sign flips between strings).
//
// Line boxes use the shared greedy wrap through the measured-advance seam
// (`text::layout_text_measured`), exactly like `paint::FontPaintTextMetrics`, so wrapping, byte
// ranges, heights and baselines follow the same algorithm and only the advance source differs.
// `word-spacing` is added by the wrap itself (never by the callback) and `text-transform` is
// applied by the wrap to the bytes it measures, so the two seams cannot double-apply either.
//
// Like `FontFace`, the measurer owns shared face state and is not safe to use from two threads at
// once (HarfBuzz shaping mutates its buffer, not the face, but the face's FreeType state is
// shared).
class FontTextMeasurer final : public TextMeasurer {
public:
    explicit FontTextMeasurer(FontFace face);
    ~FontTextMeasurer() override;

    FontTextMeasurer(const FontTextMeasurer&) = delete;
    FontTextMeasurer& operator=(const FontTextMeasurer&) = delete;
    FontTextMeasurer(FontTextMeasurer&&) noexcept;
    FontTextMeasurer& operator=(FontTextMeasurer&&) noexcept;

    [[nodiscard]] TextMetrics measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
