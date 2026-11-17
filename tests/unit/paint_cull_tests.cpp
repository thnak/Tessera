// SPDX-License-Identifier: MIT
//
// Paint-pass culling (PaintContext::cull; issue #487, docs/design/dirty-rendering.md section 6.8).
//
// The contract under test: a culled paint may omit only work that cannot reach the cull rect, so
// inside that rect the pixels and the paint commands are identical to an unculled paint, while far
// fewer nodes resolve a style. Everything below is deterministic and CPU-only - a hand-built scene,
// a recording sink, the deterministic CPU canvas, no clock, font, network or GPU.
//
// The scene deliberately contains every way a node outside the rect can still matter: a child that
// overflows a non-clipping parent into view, a child clipped away by its parent, a scroll
// container, a box shadow that reaches in from outside, wrapped text whose lines leave its own box,
// and a transformed node.

#include "paint_cache_test_support.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace paint_cull_test {

using paint_cache_test::Call;
using paint_cache_test::clear_all;
using paint_cache_test::CountingStyles;
using paint_cache_test::RecordingSink;
using paint_cache_test::same_call;
using paint_cache_test::same_calls;
using paint_cache_test::styled;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintWrap;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_cull: " << message << "\n";
    }
    return condition;
}

// Ids of the scene below.
constexpr NodeId kRoot = 1;
constexpr NodeId kOnScreen = 2;
constexpr NodeId kFarLeaf = 3;
constexpr NodeId kOverflowParent = 4;
constexpr NodeId kOverflowChild = 5;
constexpr NodeId kClippedParent = 6;
constexpr NodeId kClippedChild = 7;
constexpr NodeId kScroller = 8;
constexpr NodeId kScrollChild = 9;
constexpr NodeId kShadowed = 10;

// The cull rect used throughout: the 100x100 square at the origin. The scene is 400x400, so most of
// it is outside.
constexpr Rect kView{0.0f, 0.0f, 100.0f, 100.0f};

Node& add(Node& parent, NodeId id, Rect bounds) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_bounds(bounds);
    return node;
}

// `shadow_reach` mirrors what paint::sync_scene_style stores on the node from its computed style;
// with it false the node carries no outsets and its shadow is outside its own reach. Every painted
// position below is unique, so a test can find a draw by its origin alone.
void build_scene(Node& root, CountingStyles& styles, bool shadow_reach) {
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));

    add(root, kOnScreen, {10.0f, 10.0f, 30.0f, 30.0f});
    styles.set(kOnScreen, styled({{"background-color", "#ff0000"}}));

    add(root, kFarLeaf, {250.0f, 250.0f, 30.0f, 30.0f});
    styles.set(kFarLeaf, styled({{"background-color", "#00ff00"}}));

    // Outside the rect itself, but its child overflows back into view at (20,20): the child must
    // still be painted.
    Node& overflow = add(root, kOverflowParent, {300.0f, 300.0f, 20.0f, 20.0f});
    styles.set(kOverflowParent, styled({{"background-color", "#ffff00"}}));
    add(overflow, kOverflowChild, {-280.0f, -280.0f, 20.0f, 20.0f});
    styles.set(kOverflowChild, styled({{"background-color", "#0000ff"}}));

    // Same geometry, but the parent clips: the child's pixels are clipped away, so the whole
    // subtree may go.
    Node& clipped = add(root, kClippedParent, {200.0f, 200.0f, 20.0f, 20.0f});
    styles.set(kClippedParent, styled({{"background-color", "#ff00ff"},
                                       {"overflow-x", "hidden"},
                                       {"overflow-y", "hidden"}}));
    add(clipped, kClippedChild, {-160.0f, -160.0f, 20.0f, 20.0f});
    styles.set(kClippedChild, styled({{"background-color", "#00ffff"}}));

    Node& scroller = add(root, kScroller, {150.0f, 150.0f, 60.0f, 60.0f});
    scroller.set_scrollable_y(true);
    styles.set(kScroller, styled({{"background-color", "#303030"}}));
    add(scroller, kScrollChild, {-100.0f, -100.0f, 20.0f, 20.0f});
    styles.set(kScrollChild, styled({{"background-color", "#808080"}}));

    // Starts at x = 110, outside the rect; a 25 px shadow reaches left to x = 85, inside it.
    Node& shadowed = add(root, kShadowed, {110.0f, 10.0f, 20.0f, 20.0f});
    styles.set(kShadowed,
               styled({{"background-color", "#101010"}, {"box-shadow", "0 0 0 25px #000000"}}));
    if (shadow_reach) {
        shadowed.set_paint_outsets({25.0f, 25.0f, 25.0f, 25.0f, false});
    }
}

const Call* find_rect(const std::vector<Call>& calls, float x, float y) {
    for (const Call& call : calls) {
        if (call.kind == Call::Kind::Rect && call.rect.x == x && call.rect.y == y) {
            return &call;
        }
    }
    return nullptr;
}

bool has_rect(const std::vector<Call>& calls, float x, float y) {
    return find_rect(calls, x, y) != nullptr;
}

struct Painted {
    std::vector<Call> calls;
    PaintStats stats;
};

Painted painted(bool shadow_reach, std::optional<Rect> cull) {
    Node root(kRoot);
    CountingStyles styles;
    build_scene(root, styles, shadow_reach);
    PaintContext context;
    context.cull = cull;
    RecordingSink sink;
    Painted result;
    result.stats = paint_scene(root, styles, sink, context);
    result.calls = std::move(sink.calls);
    return result;
}

// --- Pixel equivalence on the deterministic CPU canvas -----------------------------------------

// Applies the paint pass's clip stack, so a child clipped by its parent really is clipped: without
// it the clipped children would paint in the unculled arm only, and the comparison would fail for a
// reason that is not about culling. Text is not rasterized (both arms drop it); the text rule is
// pinned by the command-level case below.
class CanvasSink final : public PaintSink {
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

    void push_clip(const RoundedClip& clip) override {
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

// Paints the scene into a canvas clipped to `kView`, exactly like the damage-clipped paint
// overload, with or without the cull rect. Both arms are clipped, so any difference is the cull's.
Render render(bool with_cull) {
    Node root(kRoot);
    CountingStyles styles;
    build_scene(root, styles, true);
    Render result;
    CanvasSink target(result.canvas);
    const std::vector<Scissor> regions{
        Scissor{static_cast<std::int32_t>(kView.x), static_cast<std::int32_t>(kView.y),
                static_cast<std::uint32_t>(kView.width), static_cast<std::uint32_t>(kView.height)}};
    ClippedPaintSink clipped(target, regions);
    PaintContext context;
    if (with_cull) {
        context.cull = kView;
    }
    result.stats = paint_scene(root, styles, clipped, context);
    return result;
}

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

tessera::rendering::rhi::Color pixel_at(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    const auto channel = [&](std::size_t offset) {
        return static_cast<float>(canvas.pixels()[index + offset]) / 255.0f;
    };
    return tessera::rendering::rhi::Color{channel(0), channel(1), channel(2), channel(3)};
}

bool test_pixels_inside_the_rect_are_unchanged() {
    const Render full = render(false);
    const Render culled = render(true);
    // Non-vacuous: the canvas shows the scene, including the child that overflows back into the
    // rect from a culled parent and the shadow that reaches in from an outside node.
    const auto red_box = pixel_at(full.canvas, 12, 12);
    const auto overflow_child = pixel_at(full.canvas, 25, 25);
    const auto shadow = pixel_at(full.canvas, 90, 10);
    const bool shows_scene = expect(red_box.r > 0.9f && red_box.b < 0.1f, "the red box paints") &&
                             expect(overflow_child.b > 0.9f && overflow_child.r < 0.1f,
                                    "the overflowing child of a culled parent paints") &&
                             expect(shadow.r < 0.2f && shadow.g < 0.2f,
                                    "the shadow of an outside node reaches into the rect");
    return shows_scene &&
           expect(mismatched_pixels(full.canvas, culled.canvas) == 0,
                  "the culled paint is pixel-identical to the unculled one inside the rect") &&
           expect(culled.stats.culled_nodes > 0, "the culled paint reports skipped nodes") &&
           expect(culled.stats.nodes < full.stats.nodes, "the culled paint resolves fewer styles");
}

// --- Command-level cases (recording sink) ------------------------------------------------------

bool test_outside_node_is_skipped_and_stays_live() {
    Node root(kRoot);
    CountingStyles styles;
    build_scene(root, styles, true);
    Node* far_leaf = nullptr;
    for (const auto& child : root.children()) {
        if (child->id() == kFarLeaf) {
            far_leaf = child.get();
        }
    }
    if (!expect(far_leaf != nullptr, "the scene has the far leaf")) {
        return false;
    }
    clear_all(root);
    far_leaf->mark_dirty(Dirty::Paint);
    PaintContext context;
    context.cull = kView;
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, context);

    // The far leaf is outside the rect: nothing of it is painted, and it is counted.
    const bool skipped = expect(!has_rect(sink.calls, 250.0f, 250.0f),
                                "a node wholly outside the rect emits nothing") &&
                         expect(stats.culled_nodes > 0, "the skipped node is counted");
    // Painting never touches the scene: the dirty flags a later frame needs are still set, and hit
    // testing still finds the node where layout put it.
    const bool live =
        expect(far_leaf->dirty() == Dirty::Paint, "the skipped node keeps its dirty flags") &&
        expect(root.hit_test(265.0f, 265.0f), "hit testing still finds the skipped node");
    // Flipping the rect to cover the node paints it again, identically to an unculled paint.
    const Painted covered = painted(true, Rect{240.0f, 240.0f, 60.0f, 60.0f});
    const Painted unculled = painted(true, std::nullopt);
    const Call* covered_leaf = find_rect(covered.calls, 250.0f, 250.0f);
    const Call* unculled_leaf = find_rect(unculled.calls, 250.0f, 250.0f);
    const bool repainted = expect(covered_leaf != nullptr, "the covering rect paints the node") &&
                           expect(unculled_leaf != nullptr, "the unculled paint paints the node") &&
                           expect(same_call(*covered_leaf, *unculled_leaf),
                                  "the repainted node is identical to the unculled paint");
    return skipped && live && repainted;
}

bool test_non_clipping_parent_keeps_its_children() {
    const Painted culled = painted(true, kView);
    return expect(culled.stats.culled_nodes > 0, "the far nodes are counted as skipped") &&
           expect(!has_rect(culled.calls, 300.0f, 300.0f),
                  "the outside parent's own box is not painted") &&
           expect(has_rect(culled.calls, 20.0f, 20.0f),
                  "its child, which overflows into the rect, is still painted");
}

bool test_clipping_parent_skips_its_subtree() {
    const Painted culled = painted(true, kView);
    return expect(culled.stats.culled_subtrees >= 2,
                  "the overflow-hidden parent and the scroll container skip their subtrees") &&
           expect(!has_rect(culled.calls, 40.0f, 40.0f), "the clipped child is not painted") &&
           expect(!has_rect(culled.calls, 50.0f, 50.0f),
                  "the scroll container's child is not painted");
}

bool test_shadow_reach_keeps_the_node() {
    const Painted with_reach = painted(true, kView);
    const Painted without_reach = painted(false, kView);
    const Painted full = painted(true, std::nullopt);
    // The shadow is the only draw at (85,-15), outside the node's own box.
    return expect(has_rect(full.calls, 85.0f, -15.0f), "the unculled paint emits the shadow") &&
           expect(has_rect(with_reach.calls, 85.0f, -15.0f),
                  "a shadow reaching into the rect keeps its node painted") &&
           expect(!has_rect(without_reach.calls, 85.0f, -15.0f),
                  "without the synced outsets the shadow is culled (the dependency is real)");
}

bool test_text_that_leaves_its_box_is_never_culled() {
    // A wrapped text node whose box sits above the rect; its later lines reach into the rect. A
    // text payload can paint outside its own box (paint_text_lines advances the line origin past
    // the content box and does not clip), so a text node's own emission is never culled on the box
    // test alone.
    Node root(kRoot);
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    CountingStyles styles;
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
    Node& text_node = add(root, 2, {20.0f, -30.0f, 60.0f, 8.0f});
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = "one two three four five six seven eight nine ten eleven twelve";
    data.wrap = PaintWrap::Wrap;
    text_node.set_paint_data(std::move(data));
    styles.set(2, styled({{"font-size", "16px"}, {"color", "#ffffff"}}));

    PaintContext context;
    context.cull = kView;
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, context);

    float lowest = -1000.0f;
    std::size_t inside = 0;
    for (const Call& call : sink.calls) {
        if (call.kind != Call::Kind::Text) {
            continue;
        }
        lowest = std::max(lowest, call.rect.bottom());
        inside += call.rect.y < kView.bottom() && call.rect.bottom() > kView.y ? 1u : 0u;
    }
    return expect(inside > 0, "a wrapped line of the text node lands inside the rect") &&
           expect(lowest > -30.0f, "the text paints below its own box") &&
           expect(stats.culled_nodes == 0, "a text node outside the rect is not culled");
}

bool test_transformed_node_is_never_culled() {
    Node root(kRoot);
    CountingStyles styles;
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    Node& moved = add(root, 2, {300.0f, 300.0f, 20.0f, 20.0f});
    moved.set_transform(tessera::rendering::rhi::Mat3::translation(10.0f, 10.0f));
    styles.set(2, styled({{"background-color", "#ff0000"}}));
    PaintContext context;
    context.cull = kView;
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, context);
    // Off-screen even with the matrix, but the node opens its own transform frame: skipping the
    // push would misplace its children, so it is painted.
    return expect(sink.clip_depth == 0, "clips stay balanced") &&
           expect(stats.transforms == 1, "the transformed node still opens its frame") &&
           expect(stats.culled_nodes == 0, "a transformed node is never culled");
}

bool test_dirty_node_with_a_cache_record_is_not_culled() {
    // Culling a dirty node would skip its re-record, and a later clean paint whose style-free key
    // matches the old entry would replay stale output. The pass therefore paints a dirty node that
    // has a cache record, and only culls it once it is clean.
    Node root(kRoot);
    CountingStyles styles;
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
    Node& far_leaf = add(root, 2, {250.0f, 250.0f, 30.0f, 30.0f});
    styles.set(2, styled({{"background-color", "#00ff00"}}));

    paint_cache_test::PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    {
        RecordingSink sink;
        (void)paint_scene(root, styles, sink, context); // records both nodes
    }
    clear_all(root);
    far_leaf.mark_dirty(Dirty::Paint);
    context.cull = kView;
    RecordingSink dirty_sink;
    const PaintStats dirty_stats = paint_scene(root, styles, dirty_sink, context);
    clear_all(root);
    RecordingSink clean_sink;
    const PaintStats clean_stats = paint_scene(root, styles, clean_sink, context);
    return expect(has_rect(dirty_sink.calls, 250.0f, 250.0f),
                  "a dirty node is painted instead of culled, so its record is refreshed") &&
           expect(dirty_stats.culled_nodes == 0, "the dirty node is not counted as culled") &&
           expect(!has_rect(clean_sink.calls, 250.0f, 250.0f),
                  "once clean and outside the rect, the node is culled") &&
           expect(clean_stats.culled_nodes == 1, "the clean node is counted as culled");
}

bool test_covering_rect_is_byte_identical() {
    const Painted full = painted(true, std::nullopt);
    const Painted covered = painted(true, Rect{-10.0f, -10.0f, 500.0f, 500.0f});
    return expect(same_calls(full.calls, covered.calls),
                  "a cull rect covering every node emits the unculled sequence") &&
           expect(covered.stats.culled_nodes == 0 && covered.stats.culled_subtrees == 0,
                  "nothing is culled");
}

} // namespace

} // namespace paint_cull_test

int main() {
    bool ok = true;
    ok = paint_cull_test::test_pixels_inside_the_rect_are_unchanged() && ok;
    ok = paint_cull_test::test_outside_node_is_skipped_and_stays_live() && ok;
    ok = paint_cull_test::test_non_clipping_parent_keeps_its_children() && ok;
    ok = paint_cull_test::test_clipping_parent_skips_its_subtree() && ok;
    ok = paint_cull_test::test_shadow_reach_keeps_the_node() && ok;
    ok = paint_cull_test::test_text_that_leaves_its_box_is_never_culled() && ok;
    ok = paint_cull_test::test_transformed_node_is_never_culled() && ok;
    ok = paint_cull_test::test_dirty_node_with_a_cache_record_is_not_culled() && ok;
    ok = paint_cull_test::test_covering_rect_is_byte_identical() && ok;
    if (!ok) {
        std::cerr << "paint_cull: failed\n";
        return 1;
    }
    std::cout << "paint_cull: ok\n";
    return 0;
}
