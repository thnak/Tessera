// SPDX-License-Identifier: MIT
//
// The paint side of layout's text-fit hint (issue #490, docs/design/dirty-rendering.md
// section 6.8). `Node::text_fits_box` is layout's answer to "does this plain-text payload stay
// inside the content box paint uses"; the paint-pass cull may trust it, because a text node's
// payload can otherwise paint outside its own box (`paint_text_lines` advances the line origin past
// the content box for every wrapped line and does not clip). This file pins both directions:
//   - a fitting plain-text node outside the cull rect is culled and counted, and the same node
//   inside
//     the rect still paints;
//   - every payload paint adds on top of the plain text layout measured - inline spans, any
//     decoration, `max_lines`, a font-size/weight override - is refused even with the hint set, as
//     is a painted rect that is no longer the layout box (a PaintView interpolation);
//   - a fitting text node inside the rect leaves every pixel and every emit byte-identical.
// The layout side (Fits / Overflows / Unknown and the content-box rule) is
// `layout_text_fit_tests.cpp`. Deterministic: stub text metrics, hand-built styles, CPU canvas.

#include "paint_cache_test_support.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace paint_cull_text_fit_test {

using paint_cache_test::Call;
using paint_cache_test::CountingStyles;
using paint_cache_test::RecordingSink;
using paint_cache_test::styled;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintSpan;
using tessera::ui::PaintWrap;
using tessera::ui::TextDecoration;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintStats;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_cull_text_fit: " << message << "\n";
    }
    return condition;
}

constexpr NodeId kRoot = 1;

// The cull rect used throughout: the 100x100 square at the origin. The scene is 400x400, so most of
// it is outside.
constexpr Rect kView{0.0f, 0.0f, 100.0f, 100.0f};

Node& add(Node& parent, NodeId id, Rect bounds) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_bounds(bounds);
    return node;
}

struct Painted {
    std::vector<Call> calls;
    PaintStats stats;
};

// --- Pixel equivalence on the deterministic CPU canvas -----------------------------------------
//
// Applies the paint pass's clip stack, so the damage-clipped comparison below is about the cull and
// not about the scissor. Text is not rasterized by the canvas (both arms drop it), so the pixel arm
// pins the box visuals of a text node; the command-level cases pin the runs.
class CanvasSink final : public tessera::ui::paint::PaintSink {
public:
    explicit CanvasSink(CpuCanvas& canvas)
        : canvas_(&canvas) {}

    void fill_rect(const Rect& rect, const tessera::rendering::rhi::Color& color) override {
        const Rect shown = clipped(rect);
        if (shown.width > 0.0f && shown.height > 0.0f) {
            canvas_->fill_rect(shown, color);
        }
    }

    void fill_rounded_rect(const Rect& rect, const tessera::rendering::rhi::Color& color,
                           float /*radius*/) override {
        fill_rect(rect, color);
    }

    void push_clip(const tessera::ui::paint::RoundedClip& clip) override {
        clips_.push_back(clip.rect);
    }

    void pop_clip() override {
        if (!clips_.empty()) {
            clips_.pop_back();
        }
    }

private:
    [[nodiscard]] Rect clipped(Rect rect) const {
        for (const Rect& clip : clips_) {
            const float left = std::max(rect.x, clip.x);
            const float top = std::max(rect.y, clip.y);
            const float right = std::min(rect.right(), clip.right());
            const float bottom = std::min(rect.bottom(), clip.bottom());
            rect =
                right > left && bottom > top ? Rect{left, top, right - left, bottom - top} : Rect{};
        }
        return rect;
    }

    CpuCanvas* canvas_ = nullptr;
    std::vector<Rect> clips_;
};

constexpr std::uint32_t kCanvasWidth = 140;
constexpr std::uint32_t kCanvasHeight = 140;

struct Render {
    CpuCanvas canvas{kCanvasWidth, kCanvasHeight};
    PaintStats stats;
};

std::size_t mismatched_pixels(const CpuCanvas& lhs, const CpuCanvas& rhs) {
    if (lhs.width() != rhs.width() || lhs.height() != rhs.height()) {
        return lhs.pixels().size();
    }
    std::size_t mismatched = 0;
    for (std::size_t index = 0; index < lhs.pixels().size(); ++index) {
        mismatched += lhs.pixels()[index] != rhs.pixels()[index] ? 1u : 0u;
    }
    return mismatched;
}

// A PaintView that interpolates every node to half its layout size: the painted rect is then the
// transformed AABB and no longer the layout box, which is the case that must not trust the fit
// fact.
class ShrunkView final : public tessera::ui::paint::PaintView {
public:
    [[nodiscard]] std::optional<tessera::rendering::rhi::Mat3>
    local_transform(NodeId) const override {
        return tessera::rendering::rhi::Mat3::scale(0.5f, 0.5f);
    }
    void overlay_style(NodeId, tessera::ui::style::ComputedStyle&) const override {}
};

const ShrunkView shrunk_view;

// --- The layout text-fit hint (issue #490) -----------------------------------------------------
//
// `Node::text_fits_box` is layout's answer to "does this plain-text payload stay inside the content
// box paint uses". Paint trusts it only for the payload it measured, so the cases below pin both
// directions: a fitting node outside the rect is culled and counted, and every payload paint adds
// to the plain text (spans, decoration, max_lines, a font-size override, or a rect that is not the
// layout box) is refused.

// A text node outside `kView` whose box and payload are set up by the caller. `fits` is what layout
// would have written; `mutate` enriches the payload after that so a test can prove the paint-side
// guard, not the layout side, is what refuses.
struct TextCase {
    Rect bounds;
    const char* text = "hi";
    bool wrap = false;
    bool fits = true;
};

Painted paint_text_case(const TextCase& test, void (*mutate)(PaintData&) = nullptr) {
    Node root(kRoot);
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    CountingStyles styles;
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
    Node& text_node = add(root, 2, test.bounds);
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = test.text;
    data.wrap = test.wrap ? PaintWrap::Wrap : PaintWrap::NoWrap;
    if (mutate != nullptr) {
        mutate(data);
    }
    text_node.set_paint_data(std::move(data));
    text_node.set_text_fits_box(test.fits);
    styles.set(2, styled({{"font-size", "16px"}, {"color", "#ffffff"}}));

    PaintContext context;
    context.cull = kView;
    RecordingSink sink;
    Painted result;
    result.stats = paint_scene(root, styles, sink, context);
    result.calls = std::move(sink.calls);
    return result;
}

std::size_t text_draws(const std::vector<Call>& calls) {
    return static_cast<std::size_t>(std::count_if(calls.begin(), calls.end(), [](const Call& call) {
        return call.kind == Call::Kind::Text;
    }));
}

bool test_a_fitting_text_node_outside_the_rect_is_culled() {
    // The stub measurer makes "hi" 16 px wide and one line tall, so it fits the 120x24 box.
    const Painted culled = paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true});
    const Painted unculled = paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, false});
    return expect(culled.stats.culled_nodes == 1,
                  "a fitting text node outside the rect is counted") &&
           expect(text_draws(culled.calls) == 0, "the culled text node emits no run") &&
           expect(text_draws(unculled.calls) > 0,
                  "without the fit fact the same node paints (the hint is what culls it)");
}

bool test_a_fitting_text_node_inside_the_rect_still_paints() {
    // The reach test runs first: a node whose box overlaps the rect is never culled, fit or not.
    const Painted painted_case =
        paint_text_case({{10.0f, 10.0f, 120.0f, 24.0f}, "hi", false, true});
    return expect(painted_case.stats.culled_nodes == 0,
                  "a text node inside the rect is not culled") &&
           expect(text_draws(painted_case.calls) > 0, "it still emits its run");
}

bool test_overflowing_text_is_not_culled() {
    // The same geometry with a wrapped paragraph: 60 lines of text in an 8 px box. Layout reports
    // `Overflows`, so the hint is false and paint must keep the node - its later lines paint below
    // the box and can reach into the rect.
    const Painted result = paint_text_case({{250.0f, 250.0f, 60.0f, 8.0f},
                                            "one two three four five six seven eight nine ten",
                                            true,
                                            false});
    return expect(result.stats.culled_nodes == 0, "overflowing text is not culled") &&
           expect(text_draws(result.calls) > 0, "its runs are emitted");
}

bool test_a_span_payload_is_not_culled() {
    // Spans paint one run per span with their own resolved style, which the plain-text measure does
    // not cover. Layout still reports `Fits` for the plain text, so this is the paint-side guard.
    const Painted result =
        paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true},
                        [](PaintData& data) { data.spans.push_back(PaintSpan{99, 0, 2}); });
    return expect(result.stats.culled_nodes == 0, "a spanned payload is not culled") &&
           expect(text_draws(result.calls) > 0, "the spanned runs are emitted");
}

bool test_an_active_decoration_is_not_culled() {
    // An active IME composition paints the uncommitted preedit run, which is not part of the
    // measured text at all; the caret/selection path also paints the line verbatim.
    const Painted result =
        paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true}, [](PaintData& data) {
            data.decoration = TextDecoration{};
            data.decoration->composition_active = true;
            data.decoration->preedit_text = "uncommitted bytes that are not in text";
        });
    return expect(result.stats.culled_nodes == 0, "a decorated payload is not culled") &&
           expect(text_draws(result.calls) > 0, "the decorated runs are emitted");
}

bool test_an_inactive_caret_is_also_refused() {
    // The caret rect is a 1-2 device px box at the line end, so it reaches past the measured line
    // even without a composition. The guard therefore refuses every decoration, not only an active
    // one.
    const Painted result =
        paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true},
                        [](PaintData& data) { data.decoration = TextDecoration{}; });
    return expect(result.stats.culled_nodes == 0, "a caret/selection payload is not culled");
}

bool test_a_max_lines_truncation_is_not_culled() {
    // Layout measured the full text; `max_lines` changes what paint emits, so the fit does not
    // describe the payload.
    const Painted result = paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true},
                                           [](PaintData& data) { data.max_lines = 1; });
    return expect(result.stats.culled_nodes == 0, "a truncated payload is not culled");
}

bool test_a_font_size_override_is_not_culled() {
    // The payload's font size re-measures the run with metrics layout never saw, so a box that fits
    // the computed size proves nothing about the override.
    const Painted result =
        paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true},
                        [](PaintData& data) { data.text_style.font_size = 64.0f; });
    return expect(result.stats.culled_nodes == 0, "a font-size override is not culled");
}

bool test_a_font_weight_override_is_not_culled() {
    const Painted result =
        paint_text_case({{250.0f, 250.0f, 120.0f, 24.0f}, "hi", false, true},
                        [](PaintData& data) { data.text_style.font_weight = 700; });
    return expect(result.stats.culled_nodes == 0, "a font-weight override is not culled");
}

bool test_an_interpolated_rect_is_not_trusted() {
    // A PaintView can move a node away from its layout box; paint then lays the text out in the
    // interpolated rect, so a fit measured against the layout box no longer describes it.
    Node root(kRoot);
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    CountingStyles styles;
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
    Node& text_node = add(root, 2, {250.0f, 250.0f, 120.0f, 24.0f});
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = "hi";
    text_node.set_paint_data(std::move(data));
    text_node.set_text_fits_box(true);
    styles.set(2, styled({{"font-size", "16px"}, {"color", "#ffffff"}}));

    PaintContext context;
    context.cull = kView;
    context.view = &shrunk_view;
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, context);
    return expect(stats.culled_nodes == 0,
                  "a node whose painted rect is not its layout box is not culled") &&
           expect(text_draws(sink.calls) > 0, "the text still paints");
}

bool test_text_pixels_inside_the_rect_are_unchanged() {
    // A fitting text node inside the rect next to one outside it: the cull must leave every pixel
    // inside the rect identical, and the inside node's runs must be byte-identical in both arms.
    const auto build = [](Node& root, CountingStyles& styles) {
        root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
        styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
        Node& inside = add(root, 2, {10.0f, 10.0f, 60.0f, 20.0f});
        PaintData inside_data;
        inside_data.kind = PaintKind::Text;
        inside_data.text = "fits";
        inside.set_paint_data(std::move(inside_data));
        inside.set_text_fits_box(true);
        styles.set(2, styled({{"font-size", "16px"}, {"color", "#ffffff"}}));
        Node& outside = add(root, 3, {250.0f, 250.0f, 60.0f, 20.0f});
        PaintData outside_data;
        outside_data.kind = PaintKind::Text;
        outside_data.text = "fits";
        outside.set_paint_data(std::move(outside_data));
        outside.set_text_fits_box(true);
        styles.set(3, styled({{"font-size", "16px"}, {"color", "#ffffff"}}));
    };
    const auto draw = [&build](bool with_cull) {
        Node root(kRoot);
        CountingStyles styles;
        build(root, styles);
        Render result;
        CanvasSink target(result.canvas);
        const std::vector<Scissor> regions{Scissor{
            static_cast<std::int32_t>(kView.x), static_cast<std::int32_t>(kView.y),
            static_cast<std::uint32_t>(kView.width), static_cast<std::uint32_t>(kView.height)}};
        ClippedPaintSink clipped(target, regions);
        PaintContext context;
        if (with_cull) {
            context.cull = kView;
        }
        result.stats = paint_scene(root, styles, clipped, context);
        return result;
    };
    const Render full = draw(false);
    const Render culled = draw(true);
    return expect(culled.stats.culled_nodes == 1, "the outside text node is the one culled") &&
           expect(mismatched_pixels(full.canvas, culled.canvas) == 0,
                  "the culled text paint is pixel-identical inside the rect");
}

} // namespace

} // namespace paint_cull_text_fit_test

int main() {
    bool ok = true;
    ok = paint_cull_text_fit_test::test_a_fitting_text_node_outside_the_rect_is_culled() && ok;
    ok = paint_cull_text_fit_test::test_a_fitting_text_node_inside_the_rect_still_paints() && ok;
    ok = paint_cull_text_fit_test::test_overflowing_text_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_a_span_payload_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_an_active_decoration_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_an_inactive_caret_is_also_refused() && ok;
    ok = paint_cull_text_fit_test::test_a_max_lines_truncation_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_a_font_size_override_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_a_font_weight_override_is_not_culled() && ok;
    ok = paint_cull_text_fit_test::test_an_interpolated_rect_is_not_trusted() && ok;
    ok = paint_cull_text_fit_test::test_text_pixels_inside_the_rect_are_unchanged() && ok;
    if (!ok) {
        std::cerr << "paint_cull_text_fit: failed\n";
        return 1;
    }
    std::cout << "paint_cull_text_fit: ok\n";
    return 0;
}
