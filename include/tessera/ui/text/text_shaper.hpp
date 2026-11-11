// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::ui::text {

// One shaped glyph in visual order. Advances and offsets are already in pixels for the
// requested style; cluster is the byte offset of the source text that produced the glyph.
struct Glyph {
    std::uint32_t id = 0;
    std::uint32_t cluster = 0;
    // Index into the font stack of the face this glyph id belongs to (0 for a single-face shaper).
    std::uint16_t face = 0;
    float advance = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
};

struct ShapedRun {
    float advance = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    bool right_to_left = false;
    // Glyphs that rendered as .notdef (id 0) because no face of the stack covers them; the host
    // reports it as a diagnostic.
    std::uint32_t missing_glyphs = 0;
    std::vector<Glyph> glyphs;
};

// A shaper maps UTF-8 text plus a style to glyphs. Bidi run splitting and line breaking stay
// outside this seam.
class TextShaper {
public:
    virtual ~TextShaper() = default;

    [[nodiscard]] virtual ShapedRun shape(std::string_view utf8, const TextStyle& style) const = 0;
};

} // namespace tessera::ui::text
