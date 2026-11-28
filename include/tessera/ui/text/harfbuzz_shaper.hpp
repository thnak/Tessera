// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_features.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <memory>
#include <span>

namespace tessera::ui::text {

// HarfBuzz maps a UTF-8 run to glyph ids, clusters, and advances. Positions are reported in
// font units by HarfBuzz and scaled to pixels here, so the shaper never mutates its font.
class HarfBuzzShaper final : public TextShaper {
public:
    explicit HarfBuzzShaper(FontFace face);
    ~HarfBuzzShaper() override;

    HarfBuzzShaper(HarfBuzzShaper&&) noexcept;
    HarfBuzzShaper& operator=(HarfBuzzShaper&&) noexcept;
    HarfBuzzShaper(const HarfBuzzShaper&) = delete;
    HarfBuzzShaper& operator=(const HarfBuzzShaper&) = delete;

    [[nodiscard]] ShapedRun shape(std::string_view utf8, const TextStyle& style) const override;

    // Shapes with a fixed direction instead of guessing it from the text, so a run of neutral
    // characters (emoji, punctuation) inside a right-to-left paragraph keeps the paragraph's order.
    [[nodiscard]] ShapedRun shape_directed(std::string_view utf8, const TextStyle& style,
                                           bool right_to_left) const;

    // Shapes with the style's whole-run features plus per-range requests, applied after them so a
    // range overrides the run-wide value of the same tag inside its bytes (docs/design/text.md
    // section 3.1). At most kMaxFontFeatureRanges ranges apply; the rest are ignored.
    [[nodiscard]] ShapedRun shape_with_ranges(std::string_view utf8, const TextStyle& style,
                                              std::span<const FontFeatureRange> ranges) const;

    [[nodiscard]] bool has_glyph(char32_t codepoint) const noexcept {
        return impl_ != nullptr && face().has_glyph(codepoint);
    }

private:
    [[nodiscard]] ShapedRun shape_impl(std::string_view utf8, const TextStyle& style, int direction,
                                       std::span<const FontFeatureRange> ranges = {}) const;
    [[nodiscard]] const FontFace& face() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
