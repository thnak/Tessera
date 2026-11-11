// SPDX-License-Identifier: MIT
//
// Dirty-only == full-redraw pixel equivalence on the deterministic CPU canvas.
//
// The first frame paints the whole scene into a persistent canvas (the "previous frame"). Each
// scripted change then goes through the real stack: scene dirty flags -> DirtyPass -> DamageTracker
// (old + new bounds) -> DamageAccumulator -> make_redraw_plan/should_full_redraw ->
// FramePipeline::paint_frame(plan), whose partial path replays every paint command clipped to the
// plan's scissor regions. The result is compared byte-for-byte with a fresh full redraw of the
// updated scene. No GPU, lavapipe, font, clock or network is involved.
//
// Cases: paint-only recolor, a layout move (old and new bounds), a shrinking resize, the area
// policy fallback and the scissor-count policy fallback. A deliberately undersized region must
// leave stale pixels, so the equality checks are not vacuous.

#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::software::CpuCanvas;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameRender;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 240;
constexpr std::uint32_t kTileCount = 9;

bool expect(bool condition, const char* case_name, const char* message) {
    if (!condition) {
        std::cerr << "damage_equivalence_cpu[" << case_name << "]: " << message << "\n";
    }
    return condition;
}

bool scissor_is(const Scissor& scissor, std::int32_t x, std::int32_t y, std::uint32_t width,
                std::uint32_t height) {
    return scissor.x == x && scissor.y == y && scissor.width == width && scissor.height == height;
}

bool has_scissor(const std::vector<Scissor>& scissors, std::int32_t x, std::int32_t y,
                 std::uint32_t width, std::uint32_t height) {
    return std::any_of(scissors.begin(), scissors.end(), [&](const Scissor& scissor) {
        return scissor_is(scissor, x, y, width, height);
    });
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

// Paint sink for the opaque, axis-aligned solid-rect scene. Rounded rects fall back to their
// bounds like the documented CPU reference (`batched2d_poc --backend cpu`); this scene has no
// radius, text, vector, image, shadow, clip or transform, so the mapping is exact.
class CpuSink final : public PaintSink {
public:
    explicit CpuSink(CpuCanvas& canvas)
        : canvas_(&canvas) {}

    void fill_rect(const Rect& rect, const Color& color) override {
        canvas_->fill_rect(rect, color);
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        canvas_->fill_rect(rect, color);
    }

private:
    CpuCanvas* canvas_ = nullptr;
};

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

void mark_subtree(Node& node) {
    node.mark_dirty(Dirty::Paint | Dirty::Render);
    for (const auto& child : node.children()) {
        mark_subtree(*child);
    }
}

struct Fixture {
    Node root{1};
    Node* header = nullptr;
    Node* card = nullptr;
    Node* badge = nullptr;
    Node* side = nullptr;
    std::array<Node*, kTileCount> tiles{};
};

Node& add_node(Node& parent, NodeId id, const Rect& bounds, const char* color, StubStyles& styles) {
    auto owner = std::make_unique<Node>(id);
    owner->set_bounds(bounds);
    Node& node = parent.add_child(std::move(owner));
    styles.set(id, styled({{"background-color", color}}));
    return node;
}

// One scene used by every case: a background, a header, a card with a child badge, an independent
// side panel and nine isolated tiles (for the scissor-count policy). All rects are integral and
// opaque so the CPU rasterizer is exact.
void build_scene(Fixture& fixture, StubStyles& styles) {
    fixture.root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1220"}}));
    fixture.header = &add_node(fixture.root, 2, {0, 0, 320, 40}, "#0F172A", styles);
    fixture.card = &add_node(fixture.root, 3, {24, 56, 80, 56}, "#38BDF8", styles);
    fixture.badge = &add_node(*fixture.card, 4, {16, 16, 32, 16}, "#FACC15", styles);
    fixture.side = &add_node(fixture.root, 5, {208, 120, 72, 64}, "#22C55E", styles);
    for (std::uint32_t index = 0; index < kTileCount; ++index) {
        fixture.tiles[index] = &add_node(
            fixture.root, static_cast<NodeId>(10 + index),
            {10.0f + static_cast<float>(index) * 30.0f, 200.0f, 12, 12}, "#94A3B8", styles);
    }
    clear_tree(fixture.root);
}

// Owns the frame stack so every case gets identical, freshly constructed state.
struct Harness {
    ManualClock clock;
    RuntimeLoop loop{clock};
    AnimationRuntime animations;
    DamageAccumulator damage{DamageConfig{Extent2D{kWidth, kHeight}, 16}};
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline{animations, loop, tracker, damage, dirty};
};

// Paints frame 0 (the previous frame) fully. Marking the whole tree dirty also seeds the tracker's
// previous bounds for every node, which is what the later old-bounds damage comes from.
void seed_previous_frame(Fixture& fixture, StubStyles& styles, Harness& harness,
                         CpuCanvas& canvas) {
    mark_subtree(fixture.root);
    harness.pipeline.tick(fixture.root, 0.016);
    harness.pipeline.finish_frame(fixture.root);
    CpuSink sink(canvas);
    harness.pipeline.paint_frame(fixture.root, styles, sink, true);
    harness.dirty.clear(fixture.root, Dirty::All);
}

CpuCanvas render_full(const Node& root, StubStyles& styles) {
    CpuCanvas canvas(kWidth, kHeight);
    CpuSink sink(canvas);
    tessera::ui::paint::paint_scene(root, styles, sink);
    return canvas;
}

struct PixelDiff {
    std::size_t mismatched = 0;
    std::size_t first = 0;
};

PixelDiff compare(const CpuCanvas& actual, const CpuCanvas& expected) {
    PixelDiff diff;
    diff.first = actual.pixels().size();
    if (actual.width() != expected.width() || actual.height() != expected.height() ||
        actual.pixels().size() != expected.pixels().size()) {
        diff.mismatched = std::max(actual.pixels().size(), expected.pixels().size());
        diff.first = 0;
        return diff;
    }
    const auto lhs = actual.pixels();
    const auto rhs = expected.pixels();
    for (std::size_t index = 0; index < lhs.size(); index += 4) {
        if (lhs[index] != rhs[index] || lhs[index + 1] != rhs[index + 1] ||
            lhs[index + 2] != rhs[index + 2] || lhs[index + 3] != rhs[index + 3]) {
            if (diff.mismatched == 0) {
                diff.first = index / 4;
            }
            ++diff.mismatched;
        }
    }
    return diff;
}

Color pixel_at(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    return Color{static_cast<float>(canvas.pixels()[index]) / 255.0f,
                 static_cast<float>(canvas.pixels()[index + 1]) / 255.0f,
                 static_cast<float>(canvas.pixels()[index + 2]) / 255.0f,
                 static_cast<float>(canvas.pixels()[index + 3]) / 255.0f};
}

// Guards against a vacuous comparison: both canvases must actually contain the scene colors the
// case expects, not just be equal blank images.
bool expect_pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, std::uint8_t r,
                  std::uint8_t g, std::uint8_t b, const char* case_name, const char* message) {
    const Color color = pixel_at(canvas, x, y);
    const auto channel = [](float value) {
        return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    };
    return expect(channel(color.r) == r && channel(color.g) == g && channel(color.b) == b &&
                      channel(color.a) == 255,
                  case_name, message);
}

bool expect_equivalent(const CpuCanvas& actual, const CpuCanvas& expected, const char* case_name) {
    const PixelDiff diff = compare(actual, expected);
    if (!expect(diff.mismatched == 0, case_name, "dirty-only pixels differ from full redraw")) {
        std::cerr << "  first mismatched pixel=" << diff.first << " mismatched=" << diff.mismatched
                  << "\n";
        return false;
    }
    return true;
}

struct PixelExpect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Paints the frame through the damage-clipped plan over the persistent canvas, checks a few
// expected scene pixels (so two equally blank images cannot pass) and compares with a fresh full
// redraw of the updated scene.
bool finish_case(Harness& harness, const Node& root, StubStyles& styles, CpuCanvas& canvas,
                 const FrameRender& render, std::initializer_list<PixelExpect> pixels,
                 const char* case_name) {
    CpuSink sink(canvas);
    if (!expect(harness.pipeline.paint_frame(root, styles, sink, render.redraw_plan).painted,
                case_name, "the frame paints")) {
        return false;
    }
    for (const PixelExpect& pixel : pixels) {
        if (!expect_pixel(canvas, pixel.x, pixel.y, pixel.r, pixel.g, pixel.b, case_name,
                          "expected scene pixel")) {
            return false;
        }
    }
    return expect_equivalent(canvas, render_full(root, styles), case_name);
}

// A paint-only recolor: one changed node, one damage region equal to its bounds. Nothing outside
// the region may change (the frame must not clear or repaint the whole target).
bool check_paint_only_recolor() {
    constexpr const char* kCase = "paint-only";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas canvas(kWidth, kHeight);
    Harness harness;
    seed_previous_frame(fixture, styles, harness, canvas);

    styles.set(3, styled({{"background-color", "#F97316"}}));
    fixture.card->mark_dirty(Dirty::Paint);
    harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender render = harness.pipeline.finish_frame(fixture.root);

    if (!expect(!render.full_redraw && !render.redraw_plan.full_redraw, kCase,
                "a small recolor stays partial") ||
        !expect(render.redraw_plan.scissors.size() == 1, kCase, "one damage region") ||
        !expect(has_scissor(render.redraw_plan.scissors, 24, 56, 80, 56), kCase,
                "the region is the changed card")) {
        return false;
    }

    return finish_case(harness, fixture.root, styles, canvas, render,
                       {{40, 70, 249, 115, 22}, {300, 220, 11, 18, 32}}, kCase);
}

// A layout move: the tracker must damage the old and the new bounds. The badge moves with its
// parent even though it is not itself dirty; both positions are inside the card's damage.
bool check_layout_move() {
    constexpr const char* kCase = "layout-move";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas canvas(kWidth, kHeight);
    Harness harness;
    seed_previous_frame(fixture, styles, harness, canvas);

    fixture.card->set_bounds({140, 88, 80, 56});
    harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender render = harness.pipeline.finish_frame(fixture.root);

    if (!expect(!render.full_redraw, kCase, "a two-region move stays partial") ||
        !expect(render.redraw_plan.scissors.size() == 2, kCase, "old and new bounds are damaged") ||
        !expect(has_scissor(render.redraw_plan.scissors, 24, 56, 80, 56), kCase,
                "the old bounds are damaged") ||
        !expect(has_scissor(render.redraw_plan.scissors, 140, 88, 80, 56), kCase,
                "the new bounds are damaged")) {
        return false;
    }

    const std::vector<std::uint8_t> previous(canvas.pixels().begin(), canvas.pixels().end());
    if (!finish_case(harness, fixture.root, styles, canvas, render,
                     {{90, 100, 11, 18, 32}, {200, 130, 56, 189, 248}}, kCase)) {
        return false;
    }
    const CpuCanvas reference = render_full(fixture.root, styles);

    // Negative control: an undersized region must leave stale pixels at the old card position, so
    // the equality above is sensitive to real under-damage.
    std::copy(previous.begin(), previous.end(), canvas.pixels().begin());
    tessera::rendering::RedrawPlan undersized;
    undersized.scissors.push_back(Scissor{24, 56, 8, 8});
    CpuSink stale_sink(canvas);
    harness.pipeline.paint_frame(fixture.root, styles, stale_sink, undersized, true);
    return expect(compare(canvas, reference).mismatched > 0, kCase,
                  "an undersized region leaves stale pixels");
}

// A resize (shrink) at the same origin: old and new bounds merge into the old box, so the partial
// repaint must restore the background where the card no longer covers it.
bool check_layout_resize() {
    constexpr const char* kCase = "resize";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas canvas(kWidth, kHeight);
    Harness harness;
    seed_previous_frame(fixture, styles, harness, canvas);

    fixture.card->set_bounds({24, 56, 60, 40});
    harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender render = harness.pipeline.finish_frame(fixture.root);

    if (!expect(!render.full_redraw, kCase, "the shrink stays partial") ||
        !expect(render.redraw_plan.scissors.size() == 1, kCase,
                "old and new bounds merge into one region") ||
        !expect(has_scissor(render.redraw_plan.scissors, 24, 56, 80, 56), kCase,
                "the merged region is the old box")) {
        return false;
    }

    return finish_case(harness, fixture.root, styles, canvas, render,
                       {{90, 100, 11, 18, 32}, {50, 66, 56, 189, 248}}, kCase);
}

// A style change on the whole-target background: the damage is the full target, so the area
// policy must take the full-redraw fallback. Its output is a plain full paint, compared exactly.
bool check_area_policy_fallback() {
    constexpr const char* kCase = "area-policy";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas canvas(kWidth, kHeight);
    Harness harness;
    seed_previous_frame(fixture, styles, harness, canvas);

    styles.set(1, styled({{"background-color", "#111827"}}));
    fixture.root.mark_dirty(Dirty::Paint);
    harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender render = harness.pipeline.finish_frame(fixture.root);

    if (!expect(render.full_redraw && render.redraw_plan.full_redraw, kCase,
                "full-target damage takes the fallback") ||
        !expect(render.redraw_plan.scissors.size() == 1, kCase, "the fallback is one region") ||
        !expect(has_scissor(render.redraw_plan.scissors, 0, 0, kWidth, kHeight), kCase,
                "the fallback region is the whole target")) {
        return false;
    }

    return finish_case(harness, fixture.root, styles, canvas, render,
                       {{300, 220, 17, 24, 39}, {40, 70, 56, 189, 248}}, kCase);
}
bool check_scissor_count_fallback() {
    constexpr const char* kCase = "scissor-policy";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas canvas(kWidth, kHeight);
    Harness harness;
    seed_previous_frame(fixture, styles, harness, canvas);

    for (Node* tile : fixture.tiles) {
        styles.set(tile->id(), styled({{"background-color", "#F43F5E"}}));
        tile->mark_dirty(Dirty::Paint);
    }
    harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender render = harness.pipeline.finish_frame(fixture.root);

    if (!expect(render.full_redraw && render.redraw_plan.full_redraw, kCase,
                "the scissor-count policy takes the fallback") ||
        !expect(render.redraw_plan.scissors.size() == 1, kCase, "the fallback is one region") ||
        !expect(has_scissor(render.redraw_plan.scissors, 0, 0, kWidth, kHeight), kCase,
                "the fallback region is the whole target")) {
        return false;
    }

    return finish_case(harness, fixture.root, styles, canvas, render,
                       {{16, 206, 244, 63, 94}, {300, 220, 11, 18, 32}}, kCase);
}

// The same script must produce the same plan and the same pixels every run (no clock, no
// iteration-order dependence in the damage/plan path).
bool check_determinism() {
    constexpr const char* kCase = "determinism";
    Fixture fixture;
    StubStyles styles;
    build_scene(fixture, styles);
    CpuCanvas first_canvas(kWidth, kHeight);
    Harness first_harness;
    seed_previous_frame(fixture, styles, first_harness, first_canvas);
    fixture.card->set_bounds({140, 88, 80, 56});
    first_harness.pipeline.tick(fixture.root, 0.016);
    const FrameRender first_render = first_harness.pipeline.finish_frame(fixture.root);
    CpuSink first_sink(first_canvas);
    first_harness.pipeline.paint_frame(fixture.root, styles, first_sink, first_render.redraw_plan);

    Fixture repeated_fixture;
    StubStyles repeated_styles;
    build_scene(repeated_fixture, repeated_styles);
    CpuCanvas second_canvas(kWidth, kHeight);
    Harness second_harness;
    seed_previous_frame(repeated_fixture, repeated_styles, second_harness, second_canvas);
    repeated_fixture.card->set_bounds({140, 88, 80, 56});
    second_harness.pipeline.tick(repeated_fixture.root, 0.016);
    const FrameRender second_render = second_harness.pipeline.finish_frame(repeated_fixture.root);
    CpuSink second_sink(second_canvas);
    second_harness.pipeline.paint_frame(repeated_fixture.root, repeated_styles, second_sink,
                                        second_render.redraw_plan);

    return expect(first_render.redraw_plan.scissors.size() ==
                          second_render.redraw_plan.scissors.size() &&
                      first_render.full_redraw == second_render.full_redraw,
                  kCase, "the plan is deterministic") &&
           expect_equivalent(second_canvas, first_canvas, kCase);
}

} // namespace

int main() {
    bool ok = true;
    ok = check_paint_only_recolor() && ok;
    ok = check_layout_move() && ok;
    ok = check_layout_resize() && ok;
    ok = check_area_policy_fallback() && ok;
    ok = check_scissor_count_fallback() && ok;
    ok = check_determinism() && ok;
    if (!ok) {
        std::cerr << "damage_equivalence_cpu: failed\n";
        return 1;
    }
    std::cout << "damage_equivalence_cpu: ok\n";
    return 0;
}
