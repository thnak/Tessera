// SPDX-License-Identifier: MIT
//
// Wrapped/multi-line paint text (D9): PaintData wrap/max_lines/text_align options resolved against
// the shared text-line-box layout, content-box wrap width (border box minus padding), per-line
// emission order/offsets/baselines, deterministic max_lines truncation, computed-style fallbacks,
// and damage-clipped replay of every line. Includes an env-gated 200-word Debug layout benchmark.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintTextAlign;
using tessera::ui::PaintWrap;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

constexpr float kFontSize = 16.0f;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_wrap: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

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

struct RecordedText {
    Rect rect;
    std::string text;
    float font_size = 0.0f;
    int font_weight = 0;
    Color color;
    std::optional<float> baseline;
    int active_clips = 0;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        ++fills;
    }
    void push_clip(const RoundedClip&) override {
        ++active_clips;
        ++clip_pushes;
    }
    void pop_clip() override {
        --active_clips;
        ++clip_pops;
    }
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text), run.font_size,
                                     run.font_weight, run.color, run.baseline, active_clips});
    }

    std::size_t fills = 0;
    int active_clips = 0;
    int clip_pushes = 0;
    int clip_pops = 0;
    std::vector<RecordedText> texts;
};

PaintData text_data(std::string text) {
    PaintData data;
    data.text = std::move(text);
    return data;
}

PaintData paragraph(std::uint32_t max_lines = 0,
                    std::optional<PaintTextAlign> align = std::nullopt) {
    PaintData data;
    data.text = "one two three four five six seven eight nine ten";
    data.wrap = PaintWrap::Wrap;
    data.max_lines = max_lines;
    data.text_align = align;
    return data;
}

std::string joined_lines(const RecordingSink& sink) {
    std::string result;
    for (const RecordedText& run : sink.texts) {
        if (!result.empty()) {
            result += ' ';
        }
        result += run.text;
    }
    return result;
}

bool test_nowrap_vs_wrap() {
    Node nowrap_node(1);
    nowrap_node.set_bounds({0, 0, 80, 60});
    nowrap_node.set_paint_data(text_data("one two three four five six seven eight"));
    Node wrap_node(2);
    wrap_node.set_bounds({0, 0, 80, 60});
    wrap_node.set_paint_data(paragraph());

    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    styles.set(2, styled({{"font-size", "16px"}}));

    RecordingSink nowrap;
    RecordingSink wrap;
    paint_scene(nowrap_node, styles, nowrap);
    paint_scene(wrap_node, styles, wrap);

    bool ok = expect(nowrap.texts.size() == 1, "NoWrap emits one run") &&
              expect(nowrap.texts[0].text == "one two three four five six seven eight",
                     "NoWrap keeps the whole text") &&
              expect(wrap.texts.size() > 1, "Wrap emits multiple lines") &&
              expect(joined_lines(wrap) == "one two three four five six seven eight nine ten",
                     "wrapped line slices reconstruct the single-spaced text");
    for (const RecordedText& run : wrap.texts) {
        ok = expect(run.font_size == kFontSize, "wrapped line keeps the resolved font size") && ok;
        ok = expect(run.rect.width <= 80.0f + 0.01f, "line width stays inside the content box") &&
             ok;
        ok = expect(run.baseline.has_value(), "wrapped line carries an absolute baseline") && ok;
    }
    return ok;
}

bool test_padding_box_width() {
    Node plain(1);
    plain.set_bounds({0, 0, 120, 80});
    plain.set_paint_data(paragraph());
    Node padded(2);
    padded.set_bounds({0, 0, 120, 80});
    padded.set_paint_data(paragraph());

    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    styles.set(2, styled({{"font-size", "16px"},
                          {"padding-left", "20px"},
                          {"padding-right", "20px"},
                          {"padding-top", "6px"},
                          {"padding-bottom", "6px"}}));

    RecordingSink plain_sink;
    RecordingSink padded_sink;
    paint_scene(plain, styles, plain_sink);
    paint_scene(padded, styles, padded_sink);

    // Content width 120 - 40 = 80, so the padded node wraps at least as often.
    bool ok =
        expect(padded_sink.texts.size() >= plain_sink.texts.size(),
               "padding narrows the wrap width") &&
        expect(padded_sink.texts[0].rect.x == 20.0f, "wrapped text starts at the content box") &&
        expect(padded_sink.texts[0].rect.y == 6.0f, "wrapped text starts below top padding");
    for (const RecordedText& run : padded_sink.texts) {
        ok = expect(run.rect.width <= 80.0f + 0.01f, "padded line fits the content width") && ok;
    }
    return ok;
}

bool test_multiline_order_and_offsets() {
    Node node(1);
    node.set_bounds({0, 0, 80, 80});
    node.set_paint_data(paragraph());
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}, {"line-height", "20px"}}));

    RecordingSink sink;
    paint_scene(node, styles, sink);
    if (!expect(sink.texts.size() >= 3, "paragraph wraps to at least three lines")) {
        return false;
    }
    bool ok = true;
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < sink.texts.size(); ++index) {
        const RecordedText& run = sink.texts[index];
        const std::size_t found =
            std::string("one two three four five six seven eight nine ten").find(run.text, cursor);
        ok = expect(found != std::string::npos, "line slices appear in source order") && ok;
        cursor = found == std::string::npos ? cursor : found + run.text.size();
        ok = expect(run.rect.y == static_cast<float>(index) * 20.0f,
                    "line tops advance by the computed line height") &&
             ok;
        ok = expect(run.baseline.value_or(0.0f) > run.rect.y &&
                        run.baseline.value_or(0.0f) < run.rect.y + run.rect.height,
                    "line baseline sits inside the line box") &&
             ok;
        if (index > 0) {
            ok = expect(run.rect.y >=
                            sink.texts[index - 1].rect.y + sink.texts[index - 1].rect.height,
                        "line boxes do not overlap") &&
                 ok;
        }
    }
    return ok;
}

bool test_max_lines_truncation() {
    Node full_node(1);
    full_node.set_bounds({0, 0, 80, 120});
    full_node.set_paint_data(paragraph());
    Node clipped_node(2);
    clipped_node.set_bounds({0, 0, 80, 120});
    clipped_node.set_paint_data(paragraph(2));

    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    styles.set(2, styled({{"font-size", "16px"}}));

    RecordingSink full;
    RecordingSink truncated;
    paint_scene(full_node, styles, full);
    paint_scene(clipped_node, styles, truncated);

    return expect(full.texts.size() > 2, "unlimited paragraph has more than two lines") &&
           expect(truncated.texts.size() == 2, "max_lines caps emitted lines") &&
           expect(truncated.texts[0].text == full.texts[0].text &&
                      truncated.texts[1].text == full.texts[1].text,
                  "truncation keeps the first computed lines deterministically");
}

bool test_alignment_and_computed_fallback() {
    Node start_node(1);
    start_node.set_bounds({0, 0, 120, 80});
    start_node.set_paint_data(paragraph(0, PaintTextAlign::Start));
    Node center_node(2);
    center_node.set_bounds({0, 0, 120, 80});
    center_node.set_paint_data(paragraph(0, PaintTextAlign::Center));
    Node end_node(3);
    end_node.set_bounds({0, 0, 120, 80});
    end_node.set_paint_data(paragraph(0, PaintTextAlign::End));
    Node computed_center(4);
    computed_center.set_bounds({0, 0, 120, 80});
    computed_center.set_paint_data(paragraph());
    Node explicit_start(5);
    explicit_start.set_bounds({0, 0, 120, 80});
    explicit_start.set_paint_data(paragraph(0, PaintTextAlign::Start));

    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    styles.set(2, styled({{"font-size", "16px"}}));
    styles.set(3, styled({{"font-size", "16px"}}));
    styles.set(4, styled({{"font-size", "16px"}, {"text-align", "center"}}));
    styles.set(5, styled({{"font-size", "16px"}, {"text-align", "center"}}));

    RecordingSink start_sink;
    RecordingSink center_sink;
    RecordingSink end_sink;
    RecordingSink computed_sink;
    RecordingSink explicit_sink;
    paint_scene(start_node, styles, start_sink);
    paint_scene(center_node, styles, center_sink);
    paint_scene(end_node, styles, end_sink);
    paint_scene(computed_center, styles, computed_sink);
    paint_scene(explicit_start, styles, explicit_sink);

    bool ok = expect(start_sink.texts[0].rect.x == 0.0f, "Start aligns at the content box") &&
              expect(center_sink.texts[0].rect.x > start_sink.texts[0].rect.x,
                     "Center shifts the first line right") &&
              expect(end_sink.texts[0].rect.x > center_sink.texts[0].rect.x,
                     "End shifts the first line further right") &&
              expect(computed_sink.texts[0].rect.x == center_sink.texts[0].rect.x,
                     "computed text-align:center applies when PaintData has no override") &&
              expect(explicit_sink.texts[0].rect.x == start_sink.texts[0].rect.x,
                     "explicit PaintData Start overrides computed center");
    return ok;
}

bool test_computed_nowrap_suppresses_wrap() {
    Node node(1);
    node.set_bounds({0, 0, 80, 60});
    node.set_paint_data(paragraph());
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}, {"white-space", "nowrap"}}));

    RecordingSink sink;
    paint_scene(node, styles, sink);
    return expect(sink.texts.size() == 1, "computed white-space:nowrap suppresses PaintData Wrap");
}

bool test_damage_clipped_multiple_lines() {
    Node root(1);
    // Tall enough that every wrapped line lies inside the damaged border box; damage replay is not
    // expected to repaint content outside the plan.
    root.set_bounds({0, 0, 80, 200});
    root.set_paint_data(paragraph());
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    root.clear_dirty(Dirty::All);
    root.mark_dirty(Dirty::Paint);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);
    pipeline.tick(root, 0.016);
    const auto render = pipeline.finish_frame(root);
    if (!expect(render.should_render && !render.full_redraw,
                "wrapped text damage plan is partial")) {
        return false;
    }

    RecordingSink full;
    paint_scene(root, styles, full);
    RecordingSink clipped;
    const FramePaint painted = pipeline.paint_frame(root, styles, clipped, render.redraw_plan);

    bool ok = expect(painted.painted, "damage-clipped wrapped paint runs") &&
              expect(full.texts.size() > 1, "full paint emits multiple lines") &&
              expect(clipped.texts.size() == full.texts.size(), "damage replay emits every line") &&
              expect(clipped.clip_pushes == static_cast<int>(clipped.texts.size()),
                     "each replayed line is clipped to the damage region") &&
              expect(clipped.clip_pushes == clipped.clip_pops, "damage text clips are balanced");
    for (std::size_t index = 0; index < clipped.texts.size() && index < full.texts.size();
         ++index) {
        ok = expect(clipped.texts[index].rect.x == full.texts[index].rect.x &&
                        clipped.texts[index].rect.y == full.texts[index].rect.y &&
                        clipped.texts[index].text == full.texts[index].text,
                    "damage replay keeps line origins and slices") &&
             ok;
    }
    return ok;
}

void benchmark_200_words() {
    std::string text;
    for (int word = 0; word < 200; ++word) {
        if (word > 0) {
            text += ' ';
        }
        text += "word" + std::to_string(word);
    }
    tessera::ui::text::TextStyle style;
    style.font_size = 16.0f;
    const tessera::ui::text::FontMetrics metrics =
        tessera::ui::text::StubFontMetricsProvider{}.metrics(style);
    tessera::ui::text::TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = 300.0f;

    constexpr int kIterations = 200;
    std::size_t lines = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const tessera::ui::text::TextLayout layout =
            tessera::ui::text::layout_text(text, style, metrics, constraints);
        lines = layout.lines.size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double microseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
        (static_cast<double>(kIterations) * 1000.0);
    std::cout << "paint_text_wrap bench: 200 words, 300px wrap -> " << lines << " lines, "
              << microseconds << " us/layout (Debug)\n";
}

} // namespace

int main(int argc, char** argv) {
    bool ok = true;
    ok = test_nowrap_vs_wrap() && ok;
    ok = test_padding_box_width() && ok;
    ok = test_multiline_order_and_offsets() && ok;
    ok = test_max_lines_truncation() && ok;
    ok = test_alignment_and_computed_fallback() && ok;
    ok = test_computed_nowrap_suppresses_wrap() && ok;
    ok = test_damage_clipped_multiple_lines() && ok;

    const bool benchmark = argc > 1 && std::string_view(argv[1]) == "--bench";
    if (benchmark) {
        benchmark_200_words();
    }
    if (!ok) {
        std::cerr << "paint_text_wrap: failed\n";
        return 1;
    }
    std::cout << "paint_text_wrap: ok\n";
    return 0;
}
