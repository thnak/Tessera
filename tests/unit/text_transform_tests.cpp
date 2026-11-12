// SPDX-License-Identifier: MIT
//
// CSS `text-transform` (docs/design/text.md section 6.5): the computed-value mapping, the shared
// ASCII 1:1 transform, the layout/measure seam (transformed bytes, original byte ranges), the paint
// output, the hit-test advance seam, and both text cache keys. Deterministic stub metrics only: no
// real font, no wall clock.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/text_layout_cache.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/hit_test.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"
#include "tessera/ui/text/text_paint.hpp"
#include "text_wrap.hpp" // private: layout_text_with_breaks with an explicit break backend

#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
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
using tessera::ui::PaintWrap;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StubPaintTextMetrics;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::paint::TextLayoutCache;
using tessera::ui::paint::TextLayoutCacheStats;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::active_backend;
using tessera::ui::text::apply_text_transform;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::hit_test_point;
using tessera::ui::text::layout_text;
using tessera::ui::text::layout_text_with_breaks;
using tessera::ui::text::text_style_from;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextMeasureCache;
using tessera::ui::text::TextMeasurer;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;
using tessera::ui::text::TextTransform;
using tessera::ui::text::WhiteSpace;

const FontMetrics kMetrics{12.8f, 3.2f, 3.2f};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "text_transform: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool same_bits(float lhs, float rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

TextStyle style(TextTransform transform, WhiteSpace mode = WhiteSpace::Normal,
                float word_spacing = 0.0f) {
    TextStyle result;
    result.font_size = 16.0f;
    result.line_height = 20.0f;
    result.white_space = mode;
    result.text_transform = transform;
    result.word_spacing = word_spacing;
    return result;
}

bool contains_lower_ascii(std::string_view run) {
    for (const char c : run) {
        if (c >= 'a' && c <= 'z') {
            return true;
        }
    }
    return false;
}

// --- 1. computed-value mapping -------------------------------------------------------------

TextTransform mapped(std::string_view value, bool present = true) {
    ComputedStyle computed;
    computed.set("font-size", "16px");
    if (present) {
        computed.set("text-transform", std::string(value));
    }
    return text_style_from(computed).text_transform;
}

bool style_mapping() {
    bool ok = expect(mapped("uppercase") == TextTransform::Uppercase, "uppercase maps") &&
              expect(mapped("lowercase") == TextTransform::Lowercase, "lowercase maps") &&
              expect(mapped("capitalize") == TextTransform::Capitalize, "capitalize maps") &&
              expect(mapped("none") == TextTransform::None, "none maps to None") &&
              expect(mapped("UPPERCASE") == TextTransform::Uppercase, "keywords are lowercased") &&
              expect(mapped(" uppercase ") == TextTransform::Uppercase, "value is trimmed");
    ok = expect(mapped("bogus") == TextTransform::None, "an unknown keyword maps to None") &&
         expect(mapped("full-width") == TextTransform::None, "unsupported CSS values stay None") &&
         expect(mapped("") == TextTransform::None, "an empty value maps to None") &&
         expect(mapped("", false) == TextTransform::None, "an absent property keeps None") && ok;
    return ok;
}

// --- 2. the shared transform ---------------------------------------------------------------

bool transform_ascii() {
    bool ok =
        expect(apply_text_transform("Hello, World! 123", TextTransform::None) ==
                   "Hello, World! 123",
               "None is the identity") &&
        expect(apply_text_transform("Hello, World! 123", TextTransform::Uppercase) ==
                   "HELLO, WORLD! 123",
               "uppercase maps a mixed string") &&
        expect(apply_text_transform("Hello, WORLD!", TextTransform::Lowercase) == "hello, world!",
               "lowercase maps a mixed string") &&
        expect(apply_text_transform("abcXYZ", TextTransform::Uppercase) == "ABCXYZ",
               "uppercase leaves A-Z alone") &&
        expect(apply_text_transform("abcXYZ", TextTransform::Lowercase) == "abcxyz",
               "lowercase leaves a-z alone");
    ok = expect(apply_text_transform("", TextTransform::Uppercase).empty() &&
                    apply_text_transform("", TextTransform::Capitalize).empty(),
                "the empty run stays empty") &&
         expect(apply_text_transform("123-_.!?", TextTransform::Capitalize) == "123-_.!?",
                "digits and punctuation never transform") &&
         ok;
    return ok;
}

bool transform_capitalize() {
    struct Case {
        std::string_view input;
        std::string_view expected;
    };
    const Case cases[] = {
        {"hello world", "Hello World"},
        {"  leading", "  Leading"},
        {"123abc", "123Abc"},
        {"(hi) there", "(Hi) There"},
        {"foo\tbar\nbaz", "Foo\tBar\nBaz"},
        {"a-b c", "A-b C"},
        {"hELLO wORLD", "HELLO WORLD"},
        {"already Capitalized", "Already Capitalized"},
    };
    bool ok = true;
    for (const Case& test : cases) {
        ok = expect(apply_text_transform(test.input, TextTransform::Capitalize) == test.expected,
                    "capitalize '" + std::string(test.input) + "'") &&
             ok;
    }
    return ok;
}

bool transform_non_ascii() {
    // U+00E9 (2 bytes), CJK (3 bytes each): copied byte for byte, while ASCII letters around them
    // still transform.
    constexpr std::string_view kEAcute = "\xC3\xA9";
    constexpr std::string_view kCjk = "\xE4\xBD\xA0\xE5\xA5\xBD";
    bool ok =
        expect(apply_text_transform(kEAcute, TextTransform::Uppercase) == kEAcute,
               "U+00E9 passes through uppercase unchanged") &&
        expect(apply_text_transform(kCjk, TextTransform::Uppercase) == kCjk,
               "CJK passes through uppercase unchanged") &&
        expect(apply_text_transform(std::string(kEAcute) + "lan", TextTransform::Uppercase) ==
                   std::string(kEAcute) + "LAN",
               "non-ASCII bytes pass through around ASCII letters") &&
        expect(apply_text_transform(std::string(kEAcute) + "lan", TextTransform::Capitalize) ==
                   std::string(kEAcute) + "Lan",
               "capitalize skips non-ASCII lead bytes") &&
        expect(apply_text_transform(std::string(kCjk) + "abc", TextTransform::Capitalize) ==
                   std::string(kCjk) + "Abc",
               "capitalize skips CJK lead bytes");
    ok = expect(apply_text_transform(kEAcute, TextTransform::Uppercase).size() == kEAcute.size(),
                "the mapping is 1:1 in bytes") &&
         ok;
    return ok;
}

bool transform_idempotent() {
    constexpr std::string_view kCorpus[] = {"hello world", "MIXED case 42", "", "\xC3\xA9lan test",
                                            "a\tb\nc"};
    bool ok = true;
    for (const std::string_view text : kCorpus) {
        for (const TextTransform transform : {TextTransform::Uppercase, TextTransform::Lowercase}) {
            const std::string once = apply_text_transform(text, transform);
            ok = expect(apply_text_transform(once, transform) == once,
                        "the mapping is idempotent") &&
                 ok;
        }
    }
    return ok;
}

// --- 3. layout and measurement -------------------------------------------------------------

// Recording advance seam: upper-case ASCII advances 10 px, everything else 8 px, so a layout that
// measured the original lowercase text would be measurably narrower than one that measured the
// transformed bytes.
struct RecordingMeasure {
    std::vector<std::string> runs;
    float lowercase_advance = 8.0f;
    float uppercase_advance = 10.0f;
};

float recording_measure(std::string_view run, const TextStyle& style, void* user) {
    auto* recorder = static_cast<RecordingMeasure*>(user);
    recorder->runs.emplace_back(run);
    float width = 0.0f;
    for (const char c : run) {
        if ((static_cast<unsigned char>(c) & 0xC0u) == 0x80u) {
            continue; // UTF-8 continuation byte: the code point was counted at its lead byte
        }
        width += (c >= 'A' && c <= 'Z') ? recorder->uppercase_advance : recorder->lowercase_advance;
    }
    return width * (style.font_size / 16.0f);
}

bool stub_layout_keeps_ranges() {
    constexpr std::string_view text = "hello world";
    const TextLayout plain =
        layout_text(text, style(TextTransform::None), kMetrics, TextConstraints{200.0f, true});
    const TextLayout upper =
        layout_text(text, style(TextTransform::Uppercase), kMetrics, TextConstraints{200.0f, true});
    bool ok = expect(upper.lines.size() == 1 && upper.lines[0].begin == 0 &&
                         upper.lines[0].end == text.size(),
                     "the stub layout keeps the original byte range") &&
              expect(near(upper.width, plain.width),
                     "the deterministic stub advance is case-independent") &&
              expect(near(upper.lines[0].width, plain.lines[0].width),
                     "the stub line width is unchanged");
    // Trimming/collapsing decisions are made on the original bytes: a transformed space is still a
    // space and a letter never becomes one.
    const TextLayout trimmed = layout_text("  hello   world  ", style(TextTransform::Uppercase),
                                           kMetrics, TextConstraints{200.0f, true});
    ok = expect(trimmed.lines.size() == 1 && trimmed.lines[0].begin == 2 &&
                    trimmed.lines[0].end == 15 && near(trimmed.width, 88.0f),
                "uppercase still trims the edge runs and collapses the interior run") &&
         ok;
    // `word-spacing` counts separators on the transformed bytes: one collapsed space, three
    // preserved ones.
    const TextLayout spaced_normal =
        layout_text("hello   world", style(TextTransform::Uppercase, WhiteSpace::Normal, 4.0f),
                    kMetrics, TextConstraints{200.0f, true});
    const TextLayout spaced_pre =
        layout_text("hello   world", style(TextTransform::Uppercase, WhiteSpace::Pre, 4.0f),
                    kMetrics, TextConstraints{200.0f, true});
    ok = expect(near(spaced_normal.width, 92.0f), "one collapsed separator widens once") &&
         expect(near(spaced_pre.width, 116.0f), "three preserved separators widen three times") &&
         ok;
    return ok;
}

bool measured_layout_sees_transformed() {
    constexpr std::string_view text = "hello world";
    RecordingMeasure recorder;
    const TextLayout layout = layout_text_with_breaks(
        text, style(TextTransform::Uppercase), kMetrics, TextConstraints{200.0f, true},
        active_backend(), &recording_measure, &recorder);
    bool ok = expect(layout.lines.size() == 1 && layout.lines[0].begin == 0 &&
                         layout.lines[0].end == text.size(),
                     "the recorded layout keeps the original byte range") &&
              expect(near(layout.width, 108.0f),
                     "the measured width follows the transformed glyph advances") &&
              expect(!recorder.runs.empty(), "the measure callback ran");
    bool saw_uppercase = false;
    for (const std::string& run : recorder.runs) {
        ok =
            expect(!contains_lower_ascii(run), "the callback receives transformed bytes: " + run) &&
            ok;
        saw_uppercase = saw_uppercase || run.find("HELLO") != std::string::npos ||
                        run.find("WORLD") != std::string::npos;
    }
    ok = expect(saw_uppercase, "the callback saw the uppercased words") && ok;

    // The transformed advance decides the wrap point too: 108 px fits exactly, 107 px does not.
    RecordingMeasure wide;
    const TextLayout fits = layout_text_with_breaks(text, style(TextTransform::Uppercase), kMetrics,
                                                    TextConstraints{108.0f, true}, active_backend(),
                                                    &recording_measure, &wide);
    RecordingMeasure narrow;
    const TextLayout wraps = layout_text_with_breaks(text, style(TextTransform::Uppercase),
                                                     kMetrics, TextConstraints{107.0f, true},
                                                     active_backend(), &recording_measure, &narrow);
    ok = expect(fits.lines.size() == 1, "108 px fits the transformed line") &&
         expect(wraps.lines.size() == 2 && wraps.lines[0].begin == 0 && wraps.lines[0].end == 5 &&
                    wraps.lines[1].begin == 6 && wraps.lines[1].end == 11,
                "the transformed width moves the wrap point, ranges still index the source") &&
         ok;
    return ok;
}

// --- 4. paint ------------------------------------------------------------------------------

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text)});
    }

    struct RecordedText {
        Rect rect;
        std::string text;
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
paint_node(std::string text, PaintWrap wrap,
           std::initializer_list<std::pair<const char*, const char*>> style) {
    Node node(1);
    node.set_bounds({0.0f, 0.0f, 200.0f, 60.0f});
    PaintData data;
    data.text = std::move(text);
    data.wrap = wrap;
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

bool paint_output() {
    // Wrapping path: the painted segments carry the transformed bytes and their advances (the
    // recorded rect widths) sum to the laid-out line width.
    const auto wrapped = paint_node(
        "hello   world", PaintWrap::Wrap,
        {{"font-size", "16px"}, {"white-space", "normal"}, {"text-transform", "uppercase"}});
    bool ok = expect(wrapped.size() == 3, "uppercase collapse paints three segments");
    std::string joined;
    float total = 0.0f;
    for (const auto& run : wrapped) {
        ok = expect(!contains_lower_ascii(run.text), "painted segment is uppercase: " + run.text) &&
             ok;
        joined += run.text;
        total += run.rect.width;
    }
    ok = expect(joined == "HELLO WORLD", "the painted sequence is the uppercased text") &&
         expect(near(total, 88.0f), "painted advances sum to the laid-out line width (88 px)") &&
         ok;
    const TextLayout reference = layout_text("hello   world", style(TextTransform::Uppercase),
                                             kMetrics, TextConstraints{200.0f, true});
    ok = expect(reference.lines.size() == 1 && near(total, reference.lines[0].width),
                "the painted total equals the measured line width") &&
         ok;

    // The whole-line fast path (no wrap, one segment) also emits transformed bytes.
    const auto fast = paint_node("hello world", PaintWrap::NoWrap,
                                 {{"font-size", "16px"}, {"text-transform", "capitalize"}});
    ok = expect(fast.size() == 1 && fast[0].text == "Hello World",
                "the single-run fast path paints the capitalized bytes") &&
         ok;
    return ok;
}

// --- 5. hit testing ------------------------------------------------------------------------

bool hit_test_advance_sees_transformed() {
    constexpr std::string_view text = "hello world";
    const TextStyle text_style = style(TextTransform::Uppercase);
    RecordingMeasure recorder;
    const auto hit = hit_test_point(text, text_style, TextConstraints{200.0f, true}, 500.0f, 1.0f,
                                    kMetrics, &recording_measure, &recorder);
    bool ok = expect(!recorder.runs.empty(), "the hit-test advance seam ran");
    for (const std::string& run : recorder.runs) {
        ok = expect(!contains_lower_ascii(run),
                    "the hit-test seam receives transformed bytes: " + run) &&
             ok;
    }
    // Byte offsets still address the original string: the right edge maps to its end.
    ok = expect(hit.byte_offset == text.size(),
                "the caret offset indexes the original text, not the transformed copy") &&
         ok;
    return ok;
}

// --- 6. caches -----------------------------------------------------------------------------

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
    (void)measure_cache.measure("hello", style(TextTransform::None), constraints);
    (void)measure_cache.measure("hello", style(TextTransform::Uppercase), constraints);
    TextMeasureCache::Stats measure_stats = measure_cache.stats();
    ok = expect(measure_stats.misses == 2 && measure_stats.entries == 2,
                "text::TextMeasureCache treats text_transform as a distinct key") &&
         ok;
    (void)measure_cache.measure("hello", style(TextTransform::None), constraints);
    measure_stats = measure_cache.stats();
    ok = expect(measure_stats.hits == 1 && measurer->calls == 2,
                "text::TextMeasureCache still hits an identical style") &&
         ok;

    TextLayoutCache layout_cache;
    const StubPaintTextMetrics stub;
    const auto get = [&](TextTransform transform) -> const TextLayout& {
        return layout_cache.get("hello", style(transform), TextConstraints{200.0f, true}, stub, 0);
    };
    const float plain = get(TextTransform::None).width;
    const float upper = get(TextTransform::Uppercase).width;
    ok = expect(same_bits(plain, upper), "the stub advance is case-independent") && ok;
    TextLayoutCacheStats layout_stats = layout_cache.stats();
    ok = expect(layout_stats.misses == 2 && layout_stats.entries == 2,
                "paint::TextLayoutCache treats text_transform as a distinct key") &&
         ok;
    (void)get(TextTransform::None);
    layout_stats = layout_cache.stats();
    ok = expect(layout_stats.hits == 1 && layout_stats.misses == 2,
                "paint::TextLayoutCache still hits an identical style") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = style_mapping() && ok;
    ok = transform_ascii() && ok;
    ok = transform_capitalize() && ok;
    ok = transform_non_ascii() && ok;
    ok = transform_idempotent() && ok;
    ok = stub_layout_keeps_ranges() && ok;
    ok = measured_layout_sees_transformed() && ok;
    ok = paint_output() && ok;
    ok = hit_test_advance_sees_transformed() && ok;
    ok = cache_keys() && ok;

    if (!ok) {
        std::cerr << "text_transform: failed\n";
        return 1;
    }
    std::cout << "text_transform: ok\n";
    return 0;
}
