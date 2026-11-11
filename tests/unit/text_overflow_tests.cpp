// SPDX-License-Identifier: MIT
//
// `text-overflow: ellipsis` (docs/design/text.md section 6.3): the style registry value, the
// paint-side truncation rules at the text layer, the end-to-end paint path, and the layout
// invariance that keeps every `clip` line box byte-identical. Deterministic stub metrics only.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyInfo;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::text::fit_ellipsis_prefix;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::kEllipsisUtf8;
using tessera::ui::text::layout_text;
using tessera::ui::text::PaintedEllipsis;
using tessera::ui::text::PaintedSegment;
using tessera::ui::text::PaintedSegmentCursor;
using tessera::ui::text::StubTextMeasurer;
using tessera::ui::text::text_style_from;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextOverflow;
using tessera::ui::text::TextStyle;
using tessera::ui::text::WhiteSpace;

const FontMetrics kMetrics{12.8f, 3.2f, 3.2f};

// Exactly the deterministic stub advance (font-size / 2 + letter spacing per code point), so every
// expected cut is a number the layout also produced.
float stub_measure(std::string_view text, const TextStyle& style, void* /*user*/) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return static_cast<float>(count) * (style.font_size * 0.5f + style.letter_spacing);
}

TextStyle style_for(WhiteSpace mode, TextOverflow overflow = TextOverflow::Clip) {
    TextStyle style;
    style.font_size = 16.0f;
    style.line_height = 20.0f;
    style.white_space = mode;
    style.text_overflow = overflow;
    return style;
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "text_overflow: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

TextLayout layout_for(std::string_view text, WhiteSpace mode, float max_width,
                      TextOverflow overflow = TextOverflow::Clip, bool wrap = false) {
    return layout_text(text, style_for(mode, overflow), kMetrics, TextConstraints{max_width, wrap});
}

// The fit exactly as paint calls it: the laid-out single line and its content-box width.
PaintedEllipsis fit_line(std::string_view text, WhiteSpace mode, float available_width) {
    const TextLayout layout = layout_for(text, mode, available_width);
    if (layout.lines.empty()) {
        return {};
    }
    const TextLine& line = layout.lines.front();
    return fit_ellipsis_prefix(text, line.begin, line.end, mode, available_width, style_for(mode),
                               &stub_measure, nullptr);
}

// Painted advance of one source range, the section 6.2 segment sequence. The range is a truncated
// line prefix, not a line edge, so it does not trim (paint's ellipsis emission behaves the same).
float painted_width(std::string_view text, std::size_t begin, std::size_t end, WhiteSpace mode) {
    PaintedSegmentCursor cursor(text, begin, end, mode, false);
    PaintedSegment segment;
    float width = 0.0f;
    while (cursor.next(segment)) {
        width += stub_measure(segment.source, style_for(mode), nullptr);
    }
    return width;
}

bool style_registry() {
    PropertyRegistry registry;
    const PropertyInfo* info = registry.find("text-overflow");
    bool ok = expect(info != nullptr && info->initial == "clip",
                     "text-overflow is registered with the initial `clip`") &&
              expect(info != nullptr && !info->inherited, "text-overflow is not inherited");
    const ComputedStyle initial = registry.initial_style();
    const std::string* value = initial.find("text-overflow");
    ok = expect(value != nullptr && *value == "clip", "the initial computed style is `clip`") && ok;

    const auto expand = [&registry](std::string_view text) {
        return registry.expand(Declaration{"text-overflow", std::string(text), false, 0});
    };
    const auto ellipsis = expand("ellipsis");
    ok = expect(ellipsis.declarations.size() == 1 && ellipsis.declarations[0].value == "ellipsis" &&
                    ellipsis.diagnostics.empty(),
                "`ellipsis` expands to one declaration") &&
         ok;
    const auto clip = expand("clip");
    ok = expect(clip.declarations.size() == 1 && clip.declarations[0].value == "clip",
                "`clip` expands to one declaration") &&
         ok;
    // The two-value `<overflow> <side>` form is rejected as a whole: nothing reaches the cascade,
    // so the computed value stays `clip` instead of half-applying one of the two keywords.
    for (const std::string_view shorthand : {"clip ellipsis", "ellipsis clip", "ellipsis left"}) {
        const auto rejected = expand(shorthand);
        ok = expect(rejected.declarations.empty(),
                    "the two-value form '" + std::string(shorthand) + "' is not applied") &&
             ok;
        ok = expect(rejected.diagnostics.size() == 1 &&
                        rejected.diagnostics[0].kind == StyleDiagnosticKind::InvalidShorthand &&
                        rejected.diagnostics[0].property == "text-overflow",
                    "rejecting '" + std::string(shorthand) + "' reports InvalidShorthand") &&
             ok;
    }

    ComputedStyle computed;
    computed.set("text-overflow", "ellipsis");
    ok = expect(text_style_from(computed).text_overflow == TextOverflow::Ellipsis,
                "computed `ellipsis` reaches TextStyle") &&
         ok;
    computed.set("text-overflow", "clip");
    ok = expect(text_style_from(computed).text_overflow == TextOverflow::Clip,
                "computed `clip` reaches TextStyle") &&
         ok;
    computed.set("text-overflow", "ellipsis clip");
    ok = expect(text_style_from(computed).text_overflow == TextOverflow::Clip,
                "a directly supplied two-value value falls back to clip") &&
         ok;
    ok = expect(text_style_from(ComputedStyle{}).text_overflow == TextOverflow::Clip,
                "an absent value is clip") &&
         ok;

    // `@supports` accepts the one-value keywords and rejects the unimplemented two-value form.
    const auto supports = [&registry](std::string_view condition) {
        const auto parsed = parse_supports_condition(condition);
        return parsed.ok && evaluate_supports(parsed.condition, registry);
    };
    ok = expect(supports("(text-overflow: ellipsis)") && supports("(text-overflow: clip)"),
                "@supports accepts clip and ellipsis") &&
         ok;
    ok = expect(!supports("(text-overflow: clip ellipsis)"),
                "@supports rejects the two-value form") &&
         ok;
    return ok;
}

bool fit_rules() {
    constexpr std::string_view text = "abcdefghij"; // 10 code points, 80 px at 16 px stub metrics
    bool ok = true;

    // Overflow: the longest prefix plus one ellipsis that still fits. 44 - 8 (ellipsis) = 36 -> 4
    // code points (32 px); the 40 px + 8 px result fits exactly and never passes the box.
    PaintedEllipsis fit = fit_line(text, WhiteSpace::NoWrap, 44.0f);
    ok = expect(fit.source_end == 4 && fit.ellipsis, "44 px keeps four characters plus ellipsis") &&
         expect(near(painted_width(text, 0, fit.source_end, WhiteSpace::NoWrap) + 8.0f, 40.0f),
                "the prefix plus ellipsis is 40 px inside a 44 px box") &&
         ok;
    // An exact total (40 px prefix budget) still paints the ellipsis: the line itself is wider.
    fit = fit_line(text, WhiteSpace::NoWrap, 40.0f);
    ok = expect(fit.source_end == 4 && fit.ellipsis, "an exact prefix + ellipsis total fits") && ok;
    // The ellipsis alone fits: the prefix is empty and paint draws only U+2026.
    fit = fit_line(text, WhiteSpace::NoWrap, 8.0f);
    ok = expect(fit.source_end == 0 && fit.ellipsis, "8 px paints only the ellipsis") && ok;
    // The ellipsis alone does not fit: longest prefix that fits, no ellipsis, no overflow.
    fit = fit_line(text, WhiteSpace::NoWrap, 4.0f);
    ok = expect(fit.source_end == 0 && !fit.ellipsis,
                "4 px has no room for the ellipsis and paints no prefix") &&
         ok;

    // Multi-byte text: the cut lands on a code point boundary, never inside U+03B1.
    constexpr std::string_view greek = "\xCE\xB1\xCE\xB2\xCE\xB3"; // three 2-byte code points
    fit = fit_line(greek, WhiteSpace::NoWrap, 20.0f); // ellipsis 8, budget 12 -> one code point
    ok = expect(fit.source_end == 2 && fit.ellipsis, "the Greek cut is 2 bytes (one code point)") &&
         ok;
    ok = expect(greek.substr(0, fit.source_end) == "\xCE\xB1",
                "the surviving prefix is one whole character") &&
         ok;

    // Cluster boundary: `e` + U+0301 is one grapheme cluster. At 24 px the whole cluster fits; the
    // cut must never be the 1-byte `e` alone.
    constexpr std::string_view combining = "e\xCC\x81x"; // e, combining acute, x
    fit = fit_line(combining, WhiteSpace::NoWrap, 24.0f);
    ok = expect(fit.source_end == 3 && fit.ellipsis,
                "the combining cluster is never split mid-cluster") &&
         ok;
    fit = fit_line(combining, WhiteSpace::NoWrap, 20.0f);
    ok = expect(fit.source_end == 0 && fit.ellipsis,
                "a cluster that does not fit leaves the prefix empty") &&
         ok;

    // A trailing collapsed whitespace run: `aaa` + one borrowed space is the last fitting prefix.
    constexpr std::string_view spaced = "aaa   bbb"; // 24 + 8 + 24 px collapsed
    fit = fit_line(spaced, WhiteSpace::NoWrap, 40.0f);
    ok = expect(fit.source_end == 6 && fit.ellipsis,
                "the collapsed run before the ellipsis is consumed whole") &&
         expect(near(painted_width(spaced, 0, fit.source_end, WhiteSpace::NoWrap), 32.0f),
                "the truncated prefix ends on the one-space segment") &&
         ok;

    // The whole result never exceeds the available width, for every width in the scan.
    for (float available = 1.0f; available <= 96.0f; available += 1.0f) {
        const PaintedEllipsis scan = fit_line(text, WhiteSpace::NoWrap, available);
        float total = painted_width(text, 0, scan.source_end, WhiteSpace::NoWrap);
        if (scan.ellipsis) {
            total += 8.0f;
        }
        ok = expect(total <= available, "painted total stays inside the content box") && ok;
    }
    return ok;
}

bool layout_invariance() {
    struct Fixture {
        std::string_view text;
        WhiteSpace mode;
        float width;
        bool wrap;
    };
    constexpr Fixture fixtures[] = {
        {"abcdefghij", WhiteSpace::NoWrap, 44.0f, false},
        {"aaa   bbb", WhiteSpace::NoWrap, 40.0f, false},
        {"abc \xD7\x90\xD7\x91\xD7\x92", WhiteSpace::NoWrap, 48.0f, false},
        {"alpha beta gamma", WhiteSpace::Normal, 40.0f, true},
        {"line\nbreak", WhiteSpace::Pre, 30.0f, false},
        {"a   b", WhiteSpace::BreakSpaces, 16.0f, true},
        {"   ", WhiteSpace::Normal, 20.0f, false},
    };
    bool ok = true;
    for (const Fixture& fixture : fixtures) {
        const TextLayout clip =
            layout_for(fixture.text, fixture.mode, fixture.width, TextOverflow::Clip, fixture.wrap);
        const TextLayout ellipsis = layout_for(fixture.text, fixture.mode, fixture.width,
                                               TextOverflow::Ellipsis, fixture.wrap);
        ok =
            expect(clip.lines.size() == ellipsis.lines.size(), "line count is identical to clip") &&
            ok;
        ok = expect(clip.width == ellipsis.width && clip.height == ellipsis.height &&
                        clip.baseline == ellipsis.baseline && clip.direction == ellipsis.direction,
                    "layout extents are byte-identical to clip") &&
             ok;
        for (std::size_t index = 0; index < clip.lines.size() && index < ellipsis.lines.size();
             ++index) {
            const TextLine& lhs = clip.lines[index];
            const TextLine& rhs = ellipsis.lines[index];
            ok = expect(lhs.begin == rhs.begin && lhs.end == rhs.end && lhs.width == rhs.width &&
                            lhs.height == rhs.height && lhs.baseline == rhs.baseline,
                        "every line box matches clip exactly") &&
                 ok;
            ok = expect(lhs.visual_runs.size() == rhs.visual_runs.size(),
                        "the bidi run count matches clip") &&
                 ok;
            for (std::size_t run = 0; run < lhs.visual_runs.size() && run < rhs.visual_runs.size();
                 ++run) {
                ok = expect(lhs.visual_runs[run].begin == rhs.visual_runs[run].begin &&
                                lhs.visual_runs[run].end == rhs.visual_runs[run].end &&
                                lhs.visual_runs[run].level == rhs.visual_runs[run].level,
                            "the bidi visual runs match clip") &&
                     ok;
            }
        }
        const StubTextMeasurer measurer;
        const TextMetrics clip_metrics = measurer.measure(
            fixture.text, style_for(fixture.mode), TextConstraints{fixture.width, fixture.wrap});
        const TextMetrics ellipsis_metrics =
            measurer.measure(fixture.text, style_for(fixture.mode, TextOverflow::Ellipsis),
                             TextConstraints{fixture.width, fixture.wrap});
        ok = expect(clip_metrics.width == ellipsis_metrics.width &&
                        clip_metrics.height == ellipsis_metrics.height &&
                        clip_metrics.baseline == ellipsis_metrics.baseline &&
                        clip_metrics.line_count == ellipsis_metrics.line_count,
                    "measured metrics are identical to clip") &&
             ok;
    }
    return ok;
}

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text), run.baseline});
    }

    struct RecordedText {
        Rect rect;
        std::string text;
        std::optional<float> baseline;
    };
    std::vector<RecordedText> texts;
};

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

std::vector<RecordingSink::RecordedText>
paint_node(std::string text, float width,
           std::initializer_list<std::pair<const char*, const char*>> style) {
    Node node(1);
    node.set_bounds({0.0f, 0.0f, width, 60.0f});
    PaintData data;
    data.text = std::move(text);
    node.set_paint_data(data);
    ComputedStyle computed;
    for (const auto& [property, value] : style) {
        computed.set(property, value);
    }
    StubStyles styles;
    styles.set(1, std::move(computed));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    return sink.texts;
}

float total_width(const std::vector<RecordingSink::RecordedText>& runs) {
    float width = 0.0f;
    for (const auto& run : runs) {
        width += run.rect.width;
    }
    return width;
}

bool paint_rules() {
    const auto paint = [](std::string text, float width, const char* overflow) {
        return paint_node(
            std::move(text), width,
            {{"font-size", "16px"}, {"white-space", "nowrap"}, {"text-overflow", overflow}});
    };
    // A line that fits exactly (80 px in an 80 px box) paints unchanged: no ellipsis, same single
    // run and box-rect behaviour as `clip`.
    auto exact = paint("abcdefghij", 80.0f, "ellipsis");
    auto exact_clip = paint("abcdefghij", 80.0f, "clip");
    bool ok =
        expect(exact.size() == 1 && exact[0].text == "abcdefghij" && exact[0].rect.width == 80.0f,
               "a line that fits exactly paints unchanged") &&
        expect(exact.size() == exact_clip.size() && exact[0].rect.width == exact_clip[0].rect.width,
               "a fitting line is painted exactly as `clip` paints it");
    // Overflows: prefix + one ellipsis, ordered and inside the box.
    auto overflow = paint("abcdefghij", 44.0f, "ellipsis");
    ok = expect(overflow.size() == 2 && overflow[0].text == "abcd" && overflow[0].rect.x == 0.0f &&
                    overflow[0].rect.width == 32.0f,
                "the prefix is the four characters that fit with the ellipsis") &&
         expect(overflow[1].text == std::string(kEllipsisUtf8) && overflow[1].rect.x == 32.0f &&
                    overflow[1].rect.width == 8.0f,
                "one ellipsis follows the prefix") &&
         expect(total_width(overflow) <= 44.0f, "the painted line never passes the box") && ok;
    // `clip` keeps today's behaviour: the whole text, the box rect, no ellipsis.
    auto clip = paint("abcdefghij", 44.0f, "clip");
    ok = expect(clip.size() == 1 && clip[0].text == "abcdefghij" && clip[0].rect.width == 44.0f,
                "clip paints the line verbatim") &&
         ok;
    // The ellipsis alone fits.
    auto alone = paint("abcdefghij", 8.0f, "ellipsis");
    ok = expect(alone.size() == 1 && alone[0].text == std::string(kEllipsisUtf8) &&
                    alone[0].rect.x == 0.0f && alone[0].rect.width == 8.0f,
                "8 px paints only the ellipsis") &&
         ok;
    // Even the ellipsis does not fit: nothing is painted and nothing overflows.
    auto nothing = paint("abcdefghij", 4.0f, "ellipsis");
    ok = expect(nothing.empty(), "4 px paints no prefix and no ellipsis") && ok;
    // A trailing collapsed whitespace run is painted as its one-space segment before the ellipsis.
    auto spaces = paint("aaa   bbb", 40.0f, "ellipsis");
    ok = expect(spaces.size() == 3 && spaces[0].text == "aaa" && spaces[1].text == " " &&
                    spaces[2].text == std::string(kEllipsisUtf8),
                "the collapsed run paints one space before the ellipsis") &&
         expect(spaces[1].rect.x == 24.0f && spaces[1].rect.width == 8.0f &&
                    spaces[2].rect.x == 32.0f,
                "the collapsed space sits between the prefix and the ellipsis") &&
         expect(near(total_width(spaces), 40.0f), "the truncated line is exactly 40 px") && ok;
    return ok;
}

bool paint_bidi() {
    // LTR paragraph with a trailing Hebrew run: 7 code points = 56 px, box 48 px. Four characters
    // (32 px) + one Hebrew code point (8 px) + the ellipsis (8 px) fits exactly; the visual run
    // order is preserved and the cut lands between two Hebrew characters, never inside one.
    constexpr std::string_view text = "abc \xD7\x90\xD7\x91\xD7\x92";
    const auto runs = paint_node(
        std::string(text), 48.0f,
        {{"font-size", "16px"}, {"white-space", "nowrap"}, {"text-overflow", "ellipsis"}});
    bool ok = expect(runs.size() >= 2, "the bidi line paints prefix runs plus the ellipsis") &&
              expect(runs.back().text == std::string(kEllipsisUtf8),
                     "the ellipsis is painted last (visual order)") &&
              expect(near(runs.back().rect.x + runs.back().rect.width, 48.0f),
                     "the bidi line ends exactly at the box edge") &&
              expect(near(total_width(runs), 48.0f), "the bidi painted advance is the box width");
    std::string painted;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index + 1 == runs.size()) {
            continue;
        }
        painted += runs[index].text;
    }
    ok = expect(painted == std::string(text.substr(0, 6)),
                "the surviving runs are the cluster-aligned source prefix in visual order") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = style_registry() && ok;
    ok = fit_rules() && ok;
    ok = layout_invariance() && ok;
    ok = paint_rules() && ok;
    ok = paint_bidi() && ok;

    if (!ok) {
        std::cerr << "text_overflow: failed\n";
        return 1;
    }
    std::cout << "text_overflow: ok\n";
    return 0;
}
