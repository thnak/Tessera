// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/font_features.hpp"
#include "tessera/ui/text/size_runs.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace tessera::ui::text {

class FontMetricsProvider;

// CSS `white-space` modes this release measures, wraps, and paints (docs/design/text.md section 6).
//
// The wrap places one UAX #14 segment per line unit; the mode decides what a collapsible
// whitespace run between two units means, whether a line may break, and whether line edges trim:
//   * Normal      - interior runs collapse to one space advance; lines wrap and both edges trim.
//   * NoWrap      - interior runs collapse the same way, but the whole text stays one line whose
//                   edges trim (CSS `nowrap`).
//   * Pre         - runs and hard newlines are preserved literally on one line (CSS `pre`).
//   * PreWrap     - interior runs advance their literal width; lines wrap and both edges trim, and
//                   a hard newline starts a new line (CSS `pre-wrap`).
//   * BreakSpaces - runs are preserved; lines wrap, and every preserved space is its own line
//                   unit, so a line may start, end, or consist only of spaces.
//
// `pre` parses to Pre and `pre-line` to Normal: the paragraph split already keeps hard newlines, so
// collapsing is the only observable part of pre-line (docs/design/text.md section 6.1).
enum class WhiteSpace : std::uint8_t {
    Normal,
    NoWrap,
    Pre,
    PreWrap,
    BreakSpaces,
};

// True when the mode wraps long lines at TextConstraints::max_width (NoWrap and Pre are single
// line).
[[nodiscard]] constexpr bool white_space_wraps(WhiteSpace mode) {
    return mode != WhiteSpace::NoWrap && mode != WhiteSpace::Pre;
}

// True when an interior collapsible run measures (and paints) as one space advance.
[[nodiscard]] constexpr bool white_space_collapses(WhiteSpace mode) {
    return mode == WhiteSpace::Normal || mode == WhiteSpace::NoWrap;
}

// True when a line trims collapsible whitespace at both edges. NoWrap is one line, so the same rule
// trims its two edges.
[[nodiscard]] constexpr bool white_space_trims_edges(WhiteSpace mode) {
    return mode == WhiteSpace::Normal || mode == WhiteSpace::NoWrap || mode == WhiteSpace::PreWrap;
}

// True when each preserved space is its own line unit (CSS break-spaces).
[[nodiscard]] constexpr bool white_space_breaks_after_spaces(WhiteSpace mode) {
    return mode == WhiteSpace::BreakSpaces;
}

// CSS `text-overflow`, paint-side only (docs/design/text.md section 6.3). The property never
// changes measurement, line boxes or baselines: every mode lays out exactly as it does with Clip.
//   * Clip     - paint the laid-out line verbatim, even when it is wider than its content box.
//   * Ellipsis - when the laid-out line does not fit, paint the longest cluster-aligned prefix plus
//                one U+2026 that still fits the content box (the layout is unchanged).
enum class TextOverflow : std::uint8_t {
    Clip,
    Ellipsis,
};

// CSS `text-transform` (docs/design/text.md section 6.5). V1 is a byte-preserving ASCII mapping, so
// every offset computed on the original text still addresses the transformed run: `A-Z`/`a-z` only,
// one byte to one byte, and non-ASCII bytes are copied unchanged. Full Unicode/ICU casing (and the
// locale-sensitive Turkic forms) is a later slice. It changes the measured glyphs, so it is part of
// both cache keys and the layout signature.
enum class TextTransform : std::uint8_t {
    None,
    Uppercase,
    Lowercase,
    Capitalize,
};

struct TextStyle {
    float font_size = 16.0f;
    float line_height = 0.0f;
    float letter_spacing = 0.0f;
    // CSS `word-spacing`: extra advance added to every word separator the layout paints — V1 counts
    // the ASCII space (U+0020) left after white-space processing. It is additive to the space's
    // glyph advance and never widens a trimmed edge space, a hard line break, a tab, or a non-ASCII
    // word separator. `normal` (and any invalid or negative computed value) maps to 0. It changes
    // line boxes, so it is part of both cache keys.
    float word_spacing = 0.0f;
    int font_weight = 400;
    bool italic = false;
    // Interned OpenType feature set from font-variant-ligatures, font-variant-numeric and
    // font-feature-settings (docs/design/text.md section 3.1). The shaper applies it, so it changes
    // glyph choice and advances and is part of every cache key. A 4-byte handle, not the list:
    // TextDraw and DrawTextOp carry it and sizeof(PaintOp) is budgeted.
    FontFeaturesId features{};
    // Inline-span font-size runs of this paragraph (docs/design/inline-size-runs.md). The wrap
    // measures each run at font_size * scale; paint draws the slices of a run at that size. Part of
    // every cache key because it changes advances and line breaks.
    TextSizeRunsId size_runs{};
    // Inline-start box of the first line, in px (docs/design/list-style-inside.md): the wrap starts
    // the first line of the text this far from the line origin and measures it as part of that
    // line, so `list-style-position: inside` can flow a marker into the text. Zero is no box. Not
    // read from a computed style: the list-item marker is the only source. It changes line breaks,
    // so it is part of every cache key and the layout signature.
    float first_line_indent = 0.0f;
    WhiteSpace white_space = WhiteSpace::Normal;
    TextTransform text_transform = TextTransform::None;
    // Paint-only: the measure cache key deliberately omits it, because it changes no glyph advance.
    TextOverflow text_overflow = TextOverflow::Clip;
    // Paint-only, like `text_overflow` (docs/design/text-decoration.md): neither changes a glyph
    // advance, a line box or a baseline, so none is part of a measure or layout cache key.
    //   * decoration_lines: `style::kDecoration*` bits of `text-decoration-line` (0 = none).
    //   * decoration_thickness: px from `text-decoration-thickness`, 0 = derive from the font size.
    //   * baseline_shift: px from `vertical-align`, positive raises the painted run.
    std::uint8_t decoration_lines = 0;
    float decoration_thickness = 0.0f;
    float baseline_shift = 0.0f;
};

struct TextConstraints {
    float max_width = std::numeric_limits<float>::infinity();
    bool wrap = true;
};

struct TextMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    int line_count = 0;
};

class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;

    [[nodiscard]] virtual TextMetrics measure(std::string_view text, const TextStyle& style,
                                              const TextConstraints& constraints) const = 0;
};

class StubTextMeasurer final : public TextMeasurer {
public:
    explicit StubTextMeasurer(
        std::shared_ptr<const FontMetricsProvider> metrics_provider = nullptr);

    [[nodiscard]] TextMetrics measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const override;

private:
    std::shared_ptr<const FontMetricsProvider> metrics_provider_;
};

TextStyle text_style_from(const style::ComputedStyle& style);

// The one `text-transform` mapping, shared by the layout/measure seams, both `advance`
// implementations, paint emission and hit testing so every path paints the same bytes.
//
// V1 maps ASCII only, 1:1 in bytes (`A-Z`/`a-z`), so a transformed run has exactly the original
// length and every byte offset computed on the source text stays valid; non-ASCII bytes are copied
// unchanged. `Capitalize` uppercases the first ASCII letter of a word, where a word starts at the
// beginning of the run or immediately after an ASCII space, tab or newline; leading digits and
// punctuation are skipped and never transformed. The function is idempotent for Uppercase and
// Lowercase (and for the same Capitalize input), so a seam may safely re-apply it.
[[nodiscard]] std::string apply_text_transform(std::string_view run, TextTransform transform);

// Number of word separators in `run`: V1 counts the ASCII space (U+0020) only, so tabs, hard line
// breaks and non-ASCII word separators (U+00A0 and friends) are not widened. Layout, paint and the
// caches share this one definition so a line's painted advance always sums to its measured width.
[[nodiscard]] std::size_t word_separator_count(std::string_view run) noexcept;

} // namespace tessera::ui::text
