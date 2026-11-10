// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"

#include <limits>
#include <memory>
#include <string_view>

namespace tessera::ui::text {

class FontMetricsProvider;

// CSS `white-space` modes this release measures and wraps (docs/design/text.md section 6).
//
// The wrap places one UAX #14 segment per line unit; the mode decides what a collapsible
// whitespace run between two units means, whether a line may break, and whether line edges trim:
//   * Normal      - interior runs collapse to one space advance; lines wrap and both edges trim.
//   * NoWrap      - the pinned single-line path: one line, separators measured literally.
//   * PreWrap     - interior runs advance their literal width; lines wrap and both edges trim.
//   * BreakSpaces - runs are preserved; lines wrap, and every preserved space is its own line
//                   unit, so a line may start, end, or consist only of spaces.
//
// `pre` parses to NoWrap (the mode already preserves runs) and `pre-line` to Normal (the paragraph
// split already keeps hard newlines, so collapsing is the only observable part of pre-line).
// Collapsing `nowrap` to one space as CSS does is deliberately not implemented: NoWrap stays the
// pre-task-110 path so the existing no-wrap fixtures and goldens are byte-identical.
enum class WhiteSpace {
    Normal,
    NoWrap,
    PreWrap,
    BreakSpaces,
};

// True when the mode wraps long lines at TextConstraints::max_width (NoWrap is the single line).
[[nodiscard]] constexpr bool white_space_wraps(WhiteSpace mode) {
    return mode != WhiteSpace::NoWrap;
}

// True when an interior collapsible run measures as one space advance.
[[nodiscard]] constexpr bool white_space_collapses(WhiteSpace mode) {
    return mode == WhiteSpace::Normal;
}

// True when a wrapped line trims collapsible whitespace at both edges.
[[nodiscard]] constexpr bool white_space_trims_edges(WhiteSpace mode) {
    return mode == WhiteSpace::Normal || mode == WhiteSpace::PreWrap;
}

// True when each preserved space is its own line unit (CSS break-spaces).
[[nodiscard]] constexpr bool white_space_breaks_after_spaces(WhiteSpace mode) {
    return mode == WhiteSpace::BreakSpaces;
}

struct TextStyle {
    float font_size = 16.0f;
    float line_height = 0.0f;
    float letter_spacing = 0.0f;
    int font_weight = 400;
    bool italic = false;
    WhiteSpace white_space = WhiteSpace::Normal;
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
