// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"

#include <limits>
#include <memory>
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
enum class WhiteSpace {
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
enum class TextOverflow {
    Clip,
    Ellipsis,
};

struct TextStyle {
    float font_size = 16.0f;
    float line_height = 0.0f;
    float letter_spacing = 0.0f;
    int font_weight = 400;
    bool italic = false;
    WhiteSpace white_space = WhiteSpace::Normal;
    // Paint-only: the measure cache key deliberately omits it, because it changes no glyph advance.
    TextOverflow text_overflow = TextOverflow::Clip;
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

} // namespace tessera::ui::text
