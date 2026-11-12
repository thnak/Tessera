// SPDX-License-Identifier: MIT
//
// CSS `word-spacing` (docs/design/text.md section 6.4): the computed-value mapping, the separator
// counter, the deterministic and measured wrap paths, both cache keys, the paint advance sum, and
// determinism. The 8 px stub advance (font-size / 2 per code point) keeps every width exact.

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/text_layout_cache.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using tessera::ui::paint::StubPaintTextMetrics;
using tessera::ui::paint::TextLayoutCache;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::layout_text;
using tessera::ui::text::layout_text_measured;
using tessera::ui::text::PaintedSegment;
using tessera::ui::text::PaintedSegmentCursor;
using tessera::ui::text::text_style_from;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextMeasureCache;
using tessera::ui::text::TextMeasurer;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;
using tessera::ui::text::WhiteSpace;

FontMetrics kMetrics{12.8f, 3.2f, 3.2f};

// Non-ASCII word separators stay at their font advance in V1. The adjacent literal form keeps the
// hex escape from swallowing the following `b`.
constexpr std::string_view kNbspText = "a\xC2\xA0"
                                       "b";
constexpr std::string_view kThinSpaceText = "a\xE2\x80\x89"
                                            "b";
constexpr std::string_view kIdeographicSpaceText = "a\xE3\x80\x80"
                                                   "b";

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "word_spacing: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool same_bits(float lhs, float rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

TextStyle style(WhiteSpace mode, float spacing) {
    TextStyle result;
    result.font_size = 16.0f;
    result.line_height = 20.0f;
    result.white_space = mode;
    result.word_spacing = spacing;
    return result;
}

// `word-spacing` after `text_style_from`; the computed font size is the relative base, like
// `letter-spacing`.
float mapped_spacing(std::string_view value, std::string_view font_size = "16px") {
    ComputedStyle computed;
    computed.set("font-size", std::string(font_size));
    computed.set("word-spacing", std::string(value));
    return text_style_from(computed).word_spacing;
}

bool style_mapping() {
    bool ok = expect(mapped_spacing("normal") == 0.0f, "`normal` computes to 0") &&
              expect(mapped_spacing("4px") == 4.0f, "`4px` computes to 4") &&
              expect(mapped_spacing("0px") == 0.0f, "`0px` computes to 0") &&
              expect(mapped_spacing("-4px") == 0.0f, "a negative length computes to 0") &&
              expect(mapped_spacing("auto") == 0.0f, "an unparsable keyword computes to 0") &&
              expect(mapped_spacing("1.2.3px") == 0.0f, "a malformed length computes to 0");
    // The forms `parse_length` accepts resolve against the computed font size.
    ok = expect(mapped_spacing("0.25em") == 4.0f, "`em` resolves against the font size") &&
         expect(mapped_spacing("25%") == 4.0f, "`%` resolves against the font size") &&
         expect(mapped_spacing("0.25rem") == 4.0f, "`rem` uses the same base as letter-spacing") &&
         expect(mapped_spacing("2px", "20px") == 2.0f, "`px` is absolute") &&
         expect(mapped_spacing("0.5em", "20px") == 10.0f, "`em` follows the computed font size") &&
         ok;
    ComputedStyle absent;
    absent.set("font-size", "20px");
    ok = expect(text_style_from(absent).word_spacing == 0.0f,
                "an absent property keeps the 0 default") &&
         ok;
    return ok;
}

bool separator_count() {
    struct Case {
        std::string_view text;
        std::size_t count;
    };
    const Case cases[] = {
        {"", 0},
        {"abc", 0},
        {"a b", 1},
        {"a b c", 2},
        {"a   b", 3},
        {"  a  ", 4},
        {" a\tb ", 2},
        {"a\tb", 0},
        {"a\fb\vb", 0},
        {"a\rb", 0},
        {"a\nb", 0},
        {"a\r\nb", 0},
        {kNbspText, 0},             // U+00A0 NO-BREAK SPACE
        {kThinSpaceText, 0},        // U+2009 THIN SPACE
        {kIdeographicSpaceText, 0}, // U+3000 IDEOGRAPHIC SPACE
    };
    bool ok = true;
    for (const Case& test : cases) {
        ok = expect(tessera::ui::text::word_separator_count(test.text) == test.count,
                    "separator count of '" + std::string(test.text) + "'") &&
             ok;
    }
    return ok;
}

float width_of(std::string_view text, WhiteSpace mode, float spacing, float max_width = 200.0f) {
    return layout_text(text, style(mode, spacing), kMetrics, TextConstraints{max_width, true})
        .width;
}

bool deterministic_layout() {
    bool ok = expect(near(width_of("a b", WhiteSpace::Normal, 0.0f), 24.0f),
                     "three stub code points are 24 px") &&
              expect(near(width_of("a b", WhiteSpace::Normal, 4.0f), 28.0f),
                     "`a b` gains exactly one spacing") &&
              expect(near(width_of("foo   bar", WhiteSpace::Normal, 4.0f), 60.0f),
                     "a collapsed run gains one spacing, not three") &&
              expect(near(width_of("foo   bar", WhiteSpace::Pre, 4.0f), 84.0f),
                     "`pre` gains one spacing per preserved space") &&
              expect(near(width_of("foo   bar", WhiteSpace::PreWrap, 4.0f), 84.0f),
                     "`pre-wrap` gains one spacing per preserved space");
    TextStyle lettered = style(WhiteSpace::Normal, 4.0f);
    lettered.letter_spacing = 1.0f;
    ok = expect(near(layout_text("a b", lettered, kMetrics, TextConstraints{200.0f, true}).width,
                     31.0f),
                "letter-spacing and word-spacing add independently") &&
         ok;
    ok = expect(near(width_of("   hello", WhiteSpace::Normal, 4.0f), 40.0f),
                "trimmed leading spaces gain none") &&
         expect(near(width_of("hello   ", WhiteSpace::Normal, 4.0f), 40.0f),
                "trimmed trailing spaces gain none") &&
         expect(near(width_of("   ", WhiteSpace::Normal, 4.0f), 0.0f),
                "a whitespace-only trimmed line gains none") &&
         ok;
    // V1 widens the ASCII space only: a collapsed tab run paints one spaced space, a preserved tab
    // and a non-ASCII word separator stay at their font advance.
    ok = expect(near(width_of("a\t\tb", WhiteSpace::Normal, 4.0f), 28.0f),
                "a collapsed tab run gains the collapsed space's one spacing") &&
         expect(near(width_of("a\t\tb", WhiteSpace::Pre, 4.0f), 32.0f),
                "preserved tabs gain no spacing") &&
         expect(near(width_of(kNbspText, WhiteSpace::Pre, 4.0f), 24.0f),
                "U+00A0 gains no spacing") &&
         ok;
    ok = expect(near(width_of("a   b", WhiteSpace::BreakSpaces, 4.0f), 52.0f),
                "each `break-spaces` space gains one spacing") &&
         ok;
    const TextLayout newline = layout_text("a\nb", style(WhiteSpace::Normal, 4.0f), kMetrics,
                                           TextConstraints{200.0f, true});
    ok = expect(newline.lines.size() == 2 && near(newline.lines[0].width, 8.0f) &&
                    near(newline.lines[1].width, 8.0f),
                "a hard newline gains no spacing") &&
         ok;
    const TextLayout empty =
        layout_text("", style(WhiteSpace::Normal, 4.0f), kMetrics, TextConstraints{200.0f, true});
    ok = expect(empty.lines.empty() && near(empty.width, 0.0f) && near(empty.height, 0.0f),
                "empty text is unchanged") &&
         ok;
    // Spacing is part of the layout, so it can move a wrap point.
    const TextLayout spaced = layout_text("foo   bar", style(WhiteSpace::Normal, 4.0f), kMetrics,
                                          TextConstraints{56.0f, true});
    const TextLayout plain = layout_text("foo   bar", style(WhiteSpace::Normal, 0.0f), kMetrics,
                                         TextConstraints{56.0f, true});
    ok = expect(plain.lines.size() == 1 && spaced.lines.size() == 2 &&
                    near(spaced.lines[0].width, 24.0f) && near(spaced.lines[1].width, 24.0f),
                "word-spacing can move a wrap point") &&
         ok;
    return ok;
}

// The raw advance seam the measured path expects: literal, `word-spacing` excluded.
float raw_measure(std::string_view text, const TextStyle& text_style, void* user) {
    const auto& metrics = *static_cast<const FontMetrics*>(user);
    TextStyle literal = text_style;
    literal.white_space = WhiteSpace::Pre;
    literal.word_spacing = 0.0f;
    TextConstraints no_wrap;
    no_wrap.wrap = false;
    return layout_text(text, literal, metrics, no_wrap).width;
}

TextLayout measured(std::string_view text, WhiteSpace mode, float spacing,
                    float max_width = 200.0f) {
    return layout_text_measured(text, style(mode, spacing), kMetrics,
                                TextConstraints{max_width, true}, &raw_measure, &kMetrics);
}

bool measured_path() {
    struct Case {
        std::string_view text;
        WhiteSpace mode;
        float spacing;
    };
    const Case cases[] = {
        {"a b", WhiteSpace::Normal, 0.0f},        {"a b", WhiteSpace::Normal, 4.0f},
        {"foo   bar", WhiteSpace::Normal, 4.0f},  {"foo   bar", WhiteSpace::Pre, 4.0f},
        {"foo   bar", WhiteSpace::PreWrap, 4.0f}, {"a\t\tb", WhiteSpace::Normal, 4.0f},
        {"   hello", WhiteSpace::Normal, 4.0f},
    };
    bool ok = true;
    for (const Case& test : cases) {
        const TextLayout expected = layout_text(test.text, style(test.mode, test.spacing), kMetrics,
                                                TextConstraints{200.0f, true});
        const TextLayout actual = measured(test.text, test.mode, test.spacing);
        ok = expect(near(actual.width, expected.width),
                    "measured width matches the deterministic width for '" +
                        std::string(test.text) + "'") &&
             ok;
        ok = expect(actual.lines.size() == expected.lines.size(),
                    "measured line count matches for '" + std::string(test.text) + "'") &&
             ok;
    }
    ok = expect(near(measured("a b", WhiteSpace::Normal, 4.0f).width, 28.0f),
                "the measured path adds the spacing too") &&
         ok;
    ok = expect(near(measured("a b", WhiteSpace::Normal, 0.0f).width + 4.0f,
                     measured("a b", WhiteSpace::Normal, 4.0f).width),
                "the measured width is the raw width plus one spacing") &&
         ok;
    // A callback that already included the spacing would double count; this one does not, and the
    // wrapped decision still matches the deterministic path.
    const TextLayout narrow_expected = layout_text("foo   bar", style(WhiteSpace::Normal, 4.0f),
                                                   kMetrics, TextConstraints{56.0f, true});
    const TextLayout narrow_actual = measured("foo   bar", WhiteSpace::Normal, 4.0f, 56.0f);
    ok = expect(narrow_actual.lines.size() == narrow_expected.lines.size() &&
                    narrow_actual.lines.size() == 2,
                "the measured path takes the same wrap decisions") &&
         ok;
    const TextLayout empty = measured("", WhiteSpace::Normal, 4.0f);
    ok = expect(empty.lines.empty() && near(empty.width, 0.0f),
                "an empty measured text stays empty") &&
         ok;
    return ok;
}

class CountingMeasurer final : public TextMeasurer {
public:
    TextMetrics measure(std::string_view text, const TextStyle&,
                        const TextConstraints&) const override {
        ++calls;
        return TextMetrics{static_cast<float>(text.size()) * 8.0f, 16.0f, 12.0f, 1};
    }

    mutable std::size_t calls = 0;
};

bool cache_keys() {
    bool ok = true;
    auto measurer = std::make_shared<CountingMeasurer>();
    TextMeasureCache measure_cache(measurer, 8);
    const TextConstraints constraints;
    (void)measure_cache.measure("a b", style(WhiteSpace::Normal, 0.0f), constraints);
    (void)measure_cache.measure("a b", style(WhiteSpace::Normal, 4.0f), constraints);
    TextMeasureCache::Stats stats = measure_cache.stats();
    ok = expect(stats.misses == 2 && stats.entries == 2,
                "text::TextMeasureCache treats word-spacing as a distinct key") &&
         ok;
    (void)measure_cache.measure("a b", style(WhiteSpace::Normal, 0.0f), constraints);
    stats = measure_cache.stats();
    ok = expect(stats.hits == 1 && stats.misses == 2 && measurer->calls == 2,
                "text::TextMeasureCache hits an identical style") &&
         ok;

    TextLayoutCache layout_cache;
    const StubPaintTextMetrics stub;
    const auto get = [&](float spacing) -> const TextLayout& {
        return layout_cache.get("a b", style(WhiteSpace::Normal, spacing),
                                TextConstraints{200.0f, true}, stub, 0);
    };
    const float plain = get(0.0f).width;
    const float spaced = get(4.0f).width;
    ok = expect(near(plain, 24.0f) && near(spaced, 28.0f),
                "paint::TextLayoutCache keeps both entries' own widths") &&
         ok;
    tessera::ui::paint::TextLayoutCacheStats paint_stats = layout_cache.stats();
    ok = expect(paint_stats.misses == 2 && paint_stats.entries == 2,
                "paint::TextLayoutCache treats word-spacing as a distinct key") &&
         ok;
    (void)get(0.0f);
    paint_stats = layout_cache.stats();
    ok = expect(paint_stats.hits == 1 && paint_stats.misses == 2,
                "paint::TextLayoutCache hits an identical style") &&
         ok;

    const float raw = stub.advance("a b", style(WhiteSpace::Normal, 0.0f));
    const float widened = stub.advance("a b", style(WhiteSpace::Normal, 4.0f));
    ok = expect(near(raw, 24.0f) && near(widened, raw + 4.0f),
                "StubPaintTextMetrics::advance adds one spacing for 'a b'") &&
         ok;
    ok = expect(near(stub.advance("foo   bar", style(WhiteSpace::Pre, 4.0f)), 84.0f),
                "StubPaintTextMetrics::advance adds one spacing per preserved space") &&
         ok;
    return ok;
}

struct PaintCase {
    std::string_view text;
    WhiteSpace mode;
    float max_width;
};

bool paint_sum() {
    const PaintCase cases[] = {
        {"foo   bar", WhiteSpace::Normal, 200.0f}, {"hello world", WhiteSpace::Normal, 200.0f},
        {"a b", WhiteSpace::Normal, 200.0f},       {"   hello", WhiteSpace::Normal, 200.0f},
        {"a\t\tb", WhiteSpace::Normal, 200.0f},    {"a\t\tb", WhiteSpace::Pre, 200.0f},
        {"foo   bar", WhiteSpace::Pre, 200.0f},    {"foo   bar", WhiteSpace::PreWrap, 56.0f},
        {"a\nb", WhiteSpace::Pre, 200.0f},         {"a   b", WhiteSpace::BreakSpaces, 16.0f},
        {kNbspText, WhiteSpace::Pre, 200.0f},
    };
    const StubPaintTextMetrics stub;
    bool ok = true;
    for (const float spacing : {0.0f, 4.0f}) {
        for (const PaintCase& test : cases) {
            const TextStyle text_style = style(test.mode, spacing);
            const TextLayout layout =
                layout_text(test.text, text_style, kMetrics, TextConstraints{test.max_width, true});
            for (const TextLine& line : layout.lines) {
                PaintedSegmentCursor cursor(test.text, line.begin, line.end, test.mode, true);
                PaintedSegment segment;
                float painted = 0.0f;
                while (cursor.next(segment)) {
                    painted += stub.advance(segment.source, text_style);
                }
                ok =
                    expect(near(painted, line.width),
                           "painted segments sum to the line width with spacing " +
                               std::to_string(spacing) + " for '" + std::string(test.text) + "'") &&
                    ok;
            }
        }
    }
    const TextLayout spaced = layout_text("a b", style(WhiteSpace::Normal, 4.0f), kMetrics,
                                          TextConstraints{200.0f, true});
    ok =
        expect(near(spaced.width, 28.0f), "the paint-sum fixture really carries the spacing") && ok;
    return ok;
}

bool same_layout(const TextLayout& lhs, const TextLayout& rhs) {
    if (lhs.lines.size() != rhs.lines.size() || !same_bits(lhs.width, rhs.width) ||
        !same_bits(lhs.height, rhs.height) || !same_bits(lhs.baseline, rhs.baseline) ||
        lhs.direction != rhs.direction) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.lines.size(); ++index) {
        const TextLine& left = lhs.lines[index];
        const TextLine& right = rhs.lines[index];
        if (!same_bits(left.width, right.width) || !same_bits(left.height, right.height) ||
            !same_bits(left.baseline, right.baseline) || left.begin != right.begin ||
            left.end != right.end || left.visual_runs != right.visual_runs) {
            return false;
        }
    }
    return true;
}

bool determinism() {
    constexpr std::string_view text = "foo   bar\nbaz\t qux";
    const TextStyle text_style = style(WhiteSpace::Normal, 4.0f);
    const TextLayout first = layout_text(text, text_style, kMetrics, TextConstraints{60.0f, true});
    const TextLayout second = layout_text(text, text_style, kMetrics, TextConstraints{60.0f, true});
    bool ok = expect(same_layout(first, second), "layout_text repeats byte-identically") &&
              expect(same_layout(measured(text, WhiteSpace::Normal, 4.0f, 60.0f),
                                 measured(text, WhiteSpace::Normal, 4.0f, 60.0f)),
                     "layout_text_measured repeats byte-identically");
    const StubPaintTextMetrics stub;
    for (const TextLine& line : first.lines) {
        PaintedSegmentCursor left_cursor(text, line.begin, line.end, WhiteSpace::Normal, true);
        PaintedSegmentCursor right_cursor(text, line.begin, line.end, WhiteSpace::Normal, true);
        PaintedSegment left;
        PaintedSegment right;
        bool more = true;
        while (more) {
            const bool left_has = left_cursor.next(left);
            const bool right_has = right_cursor.next(right);
            more = left_has && right_has;
            ok = expect(left_has == right_has &&
                            (!left_has || (left.source == right.source &&
                                           left.source_begin == right.source_begin &&
                                           left.source_end == right.source_end &&
                                           left.collapsed == right.collapsed &&
                                           same_bits(stub.advance(left.source, text_style),
                                                     stub.advance(right.source, text_style)))),
                        "painted segments and advances repeat byte-identically") &&
                 ok;
        }
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = style_mapping() && ok;
    ok = separator_count() && ok;
    ok = deterministic_layout() && ok;
    ok = measured_path() && ok;
    ok = cache_keys() && ok;
    ok = paint_sum() && ok;
    ok = determinism() && ok;

    if (!ok) {
        std::cerr << "word_spacing: failed\n";
        return 1;
    }
    std::cout << "word_spacing: ok\n";
    return 0;
}
