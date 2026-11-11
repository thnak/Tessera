// SPDX-License-Identifier: MIT
//
// Scene-level deterministic corpus for the CPU canvas sink: the paint pass emits commands, a sink
// with the documented `tools/poc/batched2d/cpu_paint_sink.hpp` semantics turns them into
// `CpuCanvas` fills, and every case asserts exact RGBA8 bytes.
//
// Covered: nested and disjoint overflow clip stacks, translate-only transforms (exact on the CPU
// path because the paint pass folds them into world bounds), rounded-rect background (the sink's
// documented bounds approximation) and rotation (the documented world-bounds AABB approximation).
// Damage-scissored replays are checked by painting the same scene full and through
// `ClippedPaintSink` per disjoint region and requiring byte identity, with an undersized-region
// negative control so the equality cannot be vacuous.
//
// No GPU, lavapipe, font, clock, or network is involved: this is the software backend's own corpus.

#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;

using Pixel = std::array<std::uint8_t, 4>;

bool expect(bool condition, const char* case_name, const char* message) {
    if (!condition) {
        std::cerr << "cpu_canvas_scene[" << case_name << "]: " << message << "\n";
    }
    return condition;
}

Pixel pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    return {canvas.pixels()[index], canvas.pixels()[index + 1], canvas.pixels()[index + 2],
            canvas.pixels()[index + 3]};
}

bool is_pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, const Pixel& expected) {
    return pixel(canvas, x, y) == expected;
}

std::size_t count_pixels(const CpuCanvas& canvas, const Pixel& expected) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < canvas.pixels().size(); index += 4) {
        if (canvas.pixels()[index] == expected[0] && canvas.pixels()[index + 1] == expected[1] &&
            canvas.pixels()[index + 2] == expected[2] &&
            canvas.pixels()[index + 3] == expected[3]) {
            ++count;
        }
    }
    return count;
}

std::size_t mismatched_pixels(const CpuCanvas& lhs, const CpuCanvas& rhs) {
    if (lhs.width() != rhs.width() || lhs.height() != rhs.height()) {
        return std::max(lhs.pixels().size(), rhs.pixels().size());
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index < lhs.pixels().size(); index += 4) {
        if (lhs.pixels()[index] != rhs.pixels()[index] ||
            lhs.pixels()[index + 1] != rhs.pixels()[index + 1] ||
            lhs.pixels()[index + 2] != rhs.pixels()[index + 2] ||
            lhs.pixels()[index + 3] != rhs.pixels()[index + 3]) {
            ++count;
        }
    }
    return count;
}

constexpr Pixel kRootPixel{16, 16, 16, 255};
constexpr Pixel kPanelPixel{32, 32, 32, 255};
constexpr Pixel kChildPixel{224, 16, 16, 255};
constexpr Pixel kAccentPixel{56, 189, 248, 255};

CpuCanvas make_canvas() {
    CpuCanvas canvas(kWidth, kHeight);
    canvas.clear(rgba8(kRootPixel[0], kRootPixel[1], kRootPixel[2]));
    return canvas;
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

    void add(NodeId id, const char* property, const char* value) {
        styles_[id].set(property, value);
    }

    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

// Mirrors the CPU golden sink (tools/poc/batched2d/cpu_paint_sink.hpp): an intersecting clip stack
// in device space, rounded rects painted as their bounds, and every fill clipped before the canvas
// sees it. It is the production mapping, kept minimal here so the corpus pins the canvas/sink
// contract rather than the proof-of-concept wrapper.
class CpuClipSink final : public PaintSink {
public:
    explicit CpuClipSink(CpuCanvas& canvas)
        : canvas_(&canvas),
          clip_{0.0f, 0.0f, static_cast<float>(canvas.width()),
                static_cast<float>(canvas.height())} {}

    void fill_rect(const Rect& rect, const Color& color) override {
        const Rect clipped = intersect(clip_, rect);
        if (clipped.width > 0.0f && clipped.height > 0.0f) {
            canvas_->fill_rect(clipped, color);
        }
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        // Documented CPU approximation: the software backend has no rounded-corner rasterizer.
        fill_rect(rect, color);
    }

    void push_clip(const RoundedClip& clip) override {
        stack_.push_back(clip_);
        clip_ = intersect(clip_, clip.rect);
    }

    void pop_clip() override {
        if (!stack_.empty()) {
            clip_ = stack_.back();
            stack_.pop_back();
        }
    }

private:
    static Rect intersect(const Rect& lhs, const Rect& rhs) {
        const float left = std::max(lhs.x, rhs.x);
        const float top = std::max(lhs.y, rhs.y);
        const float right = std::min(lhs.right(), rhs.right());
        const float bottom = std::min(lhs.bottom(), rhs.bottom());
        if (left >= right || top >= bottom) {
            return Rect{};
        }
        return Rect{left, top, right - left, bottom - top};
    }

    CpuCanvas* canvas_ = nullptr;
    Rect clip_;
    std::vector<Rect> stack_;
};

Node& add_node(Node& parent, NodeId id, const Rect& bounds, const char* background,
               StubStyles& styles) {
    auto owner = std::make_unique<Node>(id);
    owner->set_bounds(bounds);
    Node& node = parent.add_child(std::move(owner));
    if (background != nullptr) {
        styles.set(id, styled({{"background-color", background}}));
    }
    return node;
}

void clip_children(StubStyles& styles, NodeId id) {
    styles.add(id, "overflow-x", "hidden");
    styles.add(id, "overflow-y", "hidden");
}

PaintStats paint(const Node& root, StubStyles& styles, CpuCanvas& canvas) {
    CpuClipSink sink(canvas);
    return tessera::ui::paint::paint_scene(root, styles, sink);
}

// Nested overflow clips: an outer clip, a middle clip, and a child that overhangs both. The visible
// region is the three-way intersection, so pixels that escape only one of the two clips are empty.
bool check_nested_clips() {
    constexpr const char* kCase = "nested-clip";
    Node root{1};
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "#101010"}}));

    Node& outer = add_node(root, 2, {8, 8, 40, 32}, "#202020", styles);
    clip_children(styles, 2);
    Node& middle = add_node(outer, 3, {4, -8, 30, 60}, nullptr, styles);
    clip_children(styles, 3);
    add_node(middle, 4, {6, 20, 40, 40}, "#E01010", styles);

    CpuCanvas canvas = make_canvas();
    const PaintStats stats = paint(root, styles, canvas);

    // Outer clip x 8..48 y 8..40; middle adds x 12..42; the child is x 18..58 y 20..60, so the
    // visible box is x 18..42 y 20..40 = 24 x 20 pixels.
    return expect(stats.clips == 2 && stats.nodes == 4, kCase,
                  "two clips and four painted nodes") &&
           expect(count_pixels(canvas, kChildPixel) == 480, kCase,
                  "the child is clipped to the three-way intersection") &&
           expect(is_pixel(canvas, 20, 25, kChildPixel), kCase,
                  "inside both clips the child shows") &&
           expect(is_pixel(canvas, 45, 25, kPanelPixel), kCase,
                  "outside the middle clip only the outer panel shows") &&
           expect(is_pixel(canvas, 20, 45, kRootPixel) && is_pixel(canvas, 20, 5, kRootPixel),
                  kCase, "outside the outer clip the root shows") &&
           expect(is_pixel(canvas, 10, 9, kPanelPixel) && is_pixel(canvas, 50, 9, kRootPixel),
                  kCase, "the outer panel paints its own box");
}

// Disjoint clips: the clipping parent and the child have no intersection, so the child paints
// nothing at all (the empty clip must not degrade into "no clip").
bool check_disjoint_clip() {
    constexpr const char* kCase = "disjoint-clip";
    Node root{1};
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "#101010"}}));

    Node& panel = add_node(root, 2, {0, 0, 16, 16}, "#202020", styles);
    clip_children(styles, 2);
    add_node(panel, 3, {40, 40, 16, 16}, "#E01010", styles);

    CpuCanvas canvas = make_canvas();
    const PaintStats stats = paint(root, styles, canvas);

    return expect(stats.clips == 1, kCase, "the clipping parent pushes one clip") &&
           expect(count_pixels(canvas, kChildPixel) == 0, kCase,
                  "a disjoint clip drops every child pixel") &&
           expect(is_pixel(canvas, 2, 2, kPanelPixel), kCase, "the panel itself still paints") &&
           expect(is_pixel(canvas, 41, 41, kRootPixel), kCase,
                  "the child position stays root background");
}

// Rounded rects: the CPU sink paints the axis-aligned bounds. Pin the approximation (and its
// visible consequence, an unfilled corner) so a future SDF rasterizer has to update this test.
bool check_rounded_rect_is_bounds() {
    constexpr const char* kCase = "rounded-bounds";
    Node root{1};
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "#101010"}}));
    add_node(root, 2, {12, 12, 20, 20}, "#38BDF8", styles);
    for (const char* radius : {"border-top-left-radius", "border-top-right-radius",
                               "border-bottom-right-radius", "border-bottom-left-radius"}) {
        styles.add(2, radius, "8px");
    }

    CpuCanvas canvas = make_canvas();
    const PaintStats stats = paint(root, styles, canvas);

    CpuCanvas bounds_reference = make_canvas();
    bounds_reference.fill_rect({12, 12, 20, 20}, rgba8(56, 189, 248));

    return expect(stats.backgrounds == 2, kCase,
                  "the root and the rounded node each emit one background quad") &&
           expect(mismatched_pixels(canvas, bounds_reference) == 0, kCase,
                  "a rounded rect paints byte-identically to its bounds") &&
           expect(is_pixel(canvas, 12, 12, kAccentPixel), kCase,
                  "the rounded corner is filled: documented CPU gap") &&
           expect(is_pixel(canvas, 11, 11, kRootPixel), kCase, "nothing outside the bounds paints");
}

// Translate-only transforms are exact on the CPU path: the paint pass folds them into world
// bounds, so the sink receives the moved integer rect.
bool check_translation_exact() {
    constexpr const char* kCase = "translation";
    Node root{1};
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "#101010"}}));
    Node& card = add_node(root, 2, {10, 10, 20, 10}, "#38BDF8", styles);
    card.set_transform(Mat3::translation(5, 3));

    CpuCanvas canvas = make_canvas();
    paint(root, styles, canvas);

    return expect(count_pixels(canvas, kAccentPixel) == 200, kCase,
                  "the moved box keeps its area") &&
           expect(is_pixel(canvas, 15, 13, kAccentPixel) && is_pixel(canvas, 34, 22, kAccentPixel),
                  kCase, "the translate moves the painted box") &&
           expect(is_pixel(canvas, 14, 13, kRootPixel) && is_pixel(canvas, 10, 10, kRootPixel) &&
                      is_pixel(canvas, 35, 23, kRootPixel),
                  kCase, "nothing paints at the untranslated position or past the moved box");
}

// Rotation has no CPU rasterizer: the sink paints the world-bounds AABB. Pin that so the gap is
// visible (the AABB corner is filled although the rotated shape does not cover it).
bool check_rotation_is_aabb() {
    constexpr const char* kCase = "rotation-aabb";
    Node root{1};
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "#101010"}}));
    Node& card = add_node(root, 2, {20, 10, 16, 12}, "#FACC15", styles);
    card.set_transform(Mat3::rotation(0.7853981634f));

    const Rect bounds = card.world_bounds();
    CpuCanvas canvas = make_canvas();
    paint(root, styles, canvas);

    CpuCanvas aabb_reference = make_canvas();
    aabb_reference.fill_rect(bounds, rgba8(250, 204, 21));

    return expect(bounds.width > 16.0f && bounds.height > 12.0f, kCase,
                  "the AABB is larger than the unrotated box") &&
           expect(mismatched_pixels(canvas, aabb_reference) == 0, kCase,
                  "a rotated node paints exactly its world-bounds AABB") &&
           expect(is_pixel(canvas, 12, 11, Pixel{250, 204, 21, 255}), kCase,
                  "the AABB corner is filled although the rotated shape does not reach it");
}

// One scene exercising every supported primitive the damage path will replay.
void build_damage_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#101010"}}));
    add_node(root, 2, {4, 4, 20, 16}, "#38BDF8", styles);
    Node& panel = add_node(root, 3, {30, 4, 20, 16}, "#202020", styles);
    clip_children(styles, 3);
    add_node(panel, 4, {10, 10, 30, 30}, "#E01010", styles);
    Node& moved = add_node(root, 5, {10, 30, 16, 10}, "#FACC15", styles);
    moved.set_transform(Mat3::translation(3, 4));
    add_node(root, 6, {36, 28, 16, 12}, "#22C55E", styles);
    for (const char* radius : {"border-top-left-radius", "border-top-right-radius",
                               "border-bottom-right-radius", "border-bottom-left-radius"}) {
        styles.add(6, radius, "6px");
    }
}

std::vector<Scissor> tile_regions() {
    return {Scissor{0, 0, kWidth / 2, kHeight / 2}, Scissor{kWidth / 2, 0, kWidth / 2, kHeight / 2},
            Scissor{0, kHeight / 2, kWidth / 2, kHeight / 2},
            Scissor{kWidth / 2, kHeight / 2, kWidth / 2, kHeight / 2}};
}

// The damage contract on the software backend: replaying the same paint commands through disjoint
// scissor regions over the previous frame is byte-identical to a full redraw.
bool check_damage_regions_equal_full() {
    constexpr const char* kCase = "damage-regions";
    Node root{1};
    StubStyles styles;
    build_damage_scene(root, styles);

    CpuCanvas full = make_canvas();
    paint(root, styles, full);

    const std::vector<Scissor> regions = tile_regions();
    CpuCanvas tiled = make_canvas();
    CpuClipSink target(tiled);
    tessera::ui::paint::ClippedPaintSink clipped(target, regions);
    tessera::ui::paint::paint_scene(root, styles, clipped);

    if (!expect(mismatched_pixels(tiled, full) == 0, kCase,
                "per-region replay equals the full redraw byte for byte")) {
        return false;
    }

    // Negative control: one undersized region must leave stale pixels, so the equality above is
    // sensitive to real under-damage.
    CpuCanvas undersized = make_canvas();
    CpuClipSink undersized_target(undersized);
    const std::vector<Scissor> small{Scissor{0, 0, 8, 8}};
    tessera::ui::paint::ClippedPaintSink clipped_small(undersized_target, small);
    tessera::ui::paint::paint_scene(root, styles, clipped_small);
    return expect(mismatched_pixels(undersized, full) > 0, kCase,
                  "an undersized region leaves stale pixels");
}

// Repeating the same paint must produce the same bytes (no iteration-order or address dependence).
bool check_paint_determinism() {
    constexpr const char* kCase = "determinism";
    Node root{1};
    StubStyles styles;
    build_damage_scene(root, styles);

    CpuCanvas first = make_canvas();
    paint(root, styles, first);
    CpuCanvas second = make_canvas();
    paint(root, styles, second);
    return expect(mismatched_pixels(first, second) == 0, kCase, "two identical paints match") &&
           expect(count_pixels(first, kAccentPixel) == 320, kCase, "the accent card is present");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_nested_clips() && ok;
    ok = check_disjoint_clip() && ok;
    ok = check_rounded_rect_is_bounds() && ok;
    ok = check_translation_exact() && ok;
    ok = check_rotation_is_aabb() && ok;
    ok = check_damage_regions_equal_full() && ok;
    ok = check_paint_determinism() && ok;
    if (!ok) {
        std::cerr << "cpu_canvas_scene: failed\n";
        return 1;
    }
    std::cout << "cpu_canvas_scene: ok\n";
    return 0;
}
