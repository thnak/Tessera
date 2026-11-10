// SPDX-License-Identifier: MIT
//
// FramePipeline::sync_style: runs only when the tick asked for style/layout work, applies
// transform / z-index / box-shadow reach before damage is derived, and makes a style-only change
// (a new transform or shadow) produce a due frame whose damage covers the shadow.
//
// Plus the paint-only interpolation sample model (`ui/frame_interpolation.hpp`): which properties a
// tick sample captures, how two ticks blend at an alpha, and the guarantees that layout-feeding
// state is never captured and that skewed transforms hold instead of wobbling.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/style/transform.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Mat3;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::capture_paint_sample;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameTick;
using tessera::ui::frame::interpolate_paint_samples;
using tessera::ui::frame::PaintSample;
using tessera::ui::frame::PaintSamples;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ColorRgba;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pipeline_style_sync: " << message << "\n";
    }
    return condition;
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

bool sync_runs_only_on_style_or_layout_work_and_reaches_damage() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({100, 100, 50, 50});
    clear_tree(root);

    StubStyles styles;
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    // Prime the damage tracker so the next frame reports only what changes.
    box.mark_dirty(Dirty::Paint | Dirty::Render);
    pipeline.tick(root, 0.016);
    pipeline.finish_frame(root);
    clear_tree(root);

    ComputedStyle shadowed;
    shadowed.set("box-shadow", "0 0 10px #000");
    shadowed.set("z-index", "4");
    shadowed.set("transform", "translate(20px, 0)");
    styles.set(2, shadowed);

    // A quiet tick (no style/layout work) skips the sync entirely.
    FrameTick quiet = pipeline.tick(root, 0.016);
    const auto skipped = pipeline.sync_style(root, styles, quiet);
    bool ok = expect(skipped.nodes == 0 && box.z_index() == 0, "no style work, no sync");
    pipeline.finish_frame(root);
    clear_tree(root);

    // A style-dirty tick runs it, and the visual changes reach damage.
    box.mark_dirty(Dirty::Style);
    FrameTick tick = pipeline.tick(root, 0.016);
    ok = expect(tick.needs_style, "style work reported") && ok;
    const auto synced = pipeline.sync_style(root, styles, tick);
    ok = expect(synced.changed_z_index == 1 && synced.changed_transform == 1 &&
                    synced.changed_outsets == 1,
                "z-index, transform and shadow reach were applied") &&
         ok;
    const auto render = pipeline.finish_frame(root);
    ok = expect(render.should_render, "a style-only transform/shadow change makes a frame due") &&
         ok;
    // Old box (100..150, no shadow yet) and the moved box (120..170) plus its shadow reach (10):
    // damage must span x = 100 on the left to x = 180 on the right.
    bool reaches = false;
    for (const auto& scissor : render.redraw_plan.scissors) {
        reaches =
            reaches || (scissor.x <= 100 && scissor.x + static_cast<int>(scissor.width) >= 180);
    }
    ok = expect(reaches && !render.full_redraw,
                "partial damage covers the shadow and the moved box") &&
         ok;
    return ok;
}

bool forced_sync_runs_on_a_quiet_tick() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    StubStyles styles;
    ComputedStyle style;
    style.set("z-index", "2");
    styles.set(1, style);
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{100, 100}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);
    clear_tree(root);
    FrameTick quiet = pipeline.tick(root, 0.016);
    pipeline.sync_style(root, styles, quiet, true);
    return expect(root.z_index() == 2, "force overrides the quiet-tick skip");
}

// --- Paint-only interpolation sample model -------------------------------------------------------

PaintSamples sample_paint(Node& node,
                          std::initializer_list<std::pair<const char*, const char*>> values) {
    std::vector<Declaration> declarations;
    declarations.reserve(values.size());
    for (const auto& [property, value] : values) {
        declarations.push_back(Declaration{property, value, false, 0});
    }
    PaintSamples samples;
    capture_paint_sample(samples, node, declarations);
    return samples;
}

bool near(float actual, float expected, float tolerance = 1.0e-5f) {
    return std::abs(actual - expected) <= tolerance;
}

bool near_color(const ColorRgba& actual, const ColorRgba& expected, float tolerance = 1.0e-5f) {
    return near(actual.r, expected.r, tolerance) && near(actual.g, expected.g, tolerance) &&
           near(actual.b, expected.b, tolerance) && near(actual.a, expected.a, tolerance);
}

bool equal_matrix(const Mat3& actual, const Mat3& expected) {
    for (int index = 0; index < 9; ++index) {
        if (actual.m[index] != expected.m[index]) {
            return false;
        }
    }
    return true;
}

bool capture_keeps_only_paint_visible_properties() {
    Node box(2);
    box.set_bounds({0, 0, 100, 50});
    const PaintSamples samples = sample_paint(box, {{"opacity", "0.25"},
                                                    {"color", "rgb(10, 20, 30)"},
                                                    {"background-color", "#0000ff"},
                                                    {"border-top-color", "red"},
                                                    {"width", "70px"},
                                                    {"font-size", "12px"},
                                                    {"padding-left", "8px"},
                                                    {"transform", "translate(10px, 20px)"}});
    const auto found = samples.find(2);
    bool ok = expect(samples.size() == 1 && found != samples.end(),
                     "one entry for a node with paint-visible declarations");
    if (!ok) {
        return false;
    }
    const PaintSample& sample = found->second;
    const ColorRgba expected_color = tessera::ui::style::parse_color("rgb(10, 20, 30)").color;
    ok = expect(sample.opacity.has_value() && near(*sample.opacity, 0.25f), "opacity captured") &&
         ok;
    ok = expect(sample.color.has_value() && near_color(*sample.color, expected_color),
                "color captured") &&
         ok;
    ok = expect(sample.background_color.has_value() &&
                    near_color(*sample.background_color, ColorRgba{0.0f, 0.0f, 1.0f, 1.0f}),
                "background-color captured") &&
         ok;
    ok = expect(sample.border_colors[0].has_value() && !sample.border_colors[1].has_value() &&
                    !sample.border_colors[2].has_value() && !sample.border_colors[3].has_value(),
                "only the declared border color is captured") &&
         ok;
    // translate() resolves against the node box; 10px/20px is a pure offset through the default
    // 50% 50% origin, so the matrix is the identity with that translation.
    ok = expect(sample.transform.has_value() && near(sample.transform->m[0], 1.0f) &&
                    near(sample.transform->m[4], 1.0f) && near(sample.transform->m[2], 10.0f) &&
                    near(sample.transform->m[5], 20.0f),
                "transform resolved to a matrix around the box origin") &&
         ok;

    // Layout- and text-feeding declarations alone must not create an entry at all: a width or
    // font-size animation is invisible to interpolation by construction.
    const PaintSamples layout_only = sample_paint(box, {{"width", "70px"},
                                                        {"height", "20px"},
                                                        {"margin-left", "4px"},
                                                        {"font-size", "12px"},
                                                        {"line-height", "16px"}});
    ok = expect(layout_only.empty(), "layout-only declarations capture no sample") && ok;

    // Malformed or unresolvable values are dropped instead of being guessed.
    const PaintSamples invalid = sample_paint(box, {{"opacity", "oops"},
                                                    {"background-color", "not-a-color"},
                                                    {"border-left-color", "currentColor"},
                                                    {"transform", "bogus(1)"}});
    ok = expect(invalid.empty(), "malformed paint declarations capture no sample") && ok;

    const PaintSamples none = sample_paint(box, {{"transform", "none"}});
    ok = expect(none.size() == 1 && none.at(2).transform.has_value() &&
                    equal_matrix(*none.at(2).transform, Mat3::identity()),
                "transform: none is an identity sample") &&
         ok;

    const PaintSamples percent = sample_paint(box, {{"transform", "translate(50%, -10%)"}});
    ok = expect(percent.size() == 1 && near(percent.at(2).transform->m[2], 50.0f) &&
                    near(percent.at(2).transform->m[5], -5.0f),
                "percentages resolve against the node box") &&
         ok;
    return ok;
}

bool interpolation_blends_values_and_colors() {
    Node box(2);
    box.set_bounds({0, 0, 100, 50});
    const PaintSamples previous = sample_paint(box, {{"opacity", "0"},
                                                     {"color", "rgb(0, 0, 0)"},
                                                     {"background-color", "rgba(0, 0, 0, 0)"},
                                                     {"transform", "translate(0px, 0px)"}});
    const PaintSamples current = sample_paint(box, {{"opacity", "1"},
                                                    {"color", "rgb(255, 255, 255)"},
                                                    {"background-color", "rgba(100, 200, 0, 1)"},
                                                    {"transform", "translate(40px, 20px)"}});

    const PaintSamples at_zero = interpolate_paint_samples(previous, current, 0.0);
    const PaintSamples mid = interpolate_paint_samples(previous, current, 0.5);
    const PaintSamples at_one = interpolate_paint_samples(previous, current, 1.0);
    bool ok = expect(at_zero.size() == 1 && mid.size() == 1 && at_one.size() == 1,
                     "interpolation keeps exactly the current tick's nodes");
    if (!ok) {
        return false;
    }
    const PaintSample& zero = at_zero.at(2);
    const PaintSample& half = mid.at(2);
    const PaintSample& one = at_one.at(2);
    ok = expect(near(*zero.opacity, 0.0f) && near(*half.opacity, 0.5f) && near(*one.opacity, 1.0f),
                "alpha 0/0.5/1 blend opacity from previous to current") &&
         ok;
    ok = expect(near(zero.transform->m[2], 0.0f) && near(zero.transform->m[5], 0.0f) &&
                    near(half.transform->m[2], 20.0f) && near(half.transform->m[5], 10.0f) &&
                    near(one.transform->m[2], 40.0f) && near(one.transform->m[5], 20.0f),
                "alpha 0/0.5/1 blend the transform translation") &&
         ok;
    ok = expect(near_color(*zero.color, ColorRgba{0.0f, 0.0f, 0.0f, 1.0f}) &&
                    near_color(*half.color, ColorRgba{0.5f, 0.5f, 0.5f, 1.0f}) &&
                    near_color(*one.color, ColorRgba{1.0f, 1.0f, 1.0f, 1.0f}),
                "alpha 0/0.5/1 blend color in sRGB") &&
         ok;
    const ColorRgba background = tessera::ui::style::parse_color("rgba(100, 200, 0, 1)").color;
    ok = expect(near(half.background_color->r, background.r * 0.5f) &&
                    near(half.background_color->g, background.g * 0.5f) &&
                    near(half.background_color->a, 0.5f) &&
                    near_color(*one.background_color, background),
                "background alpha blends to the halfway value") &&
         ok;

    // Out-of-range and broken alphas clamp; alpha 0 never extrapolates backwards.
    const PaintSamples below = interpolate_paint_samples(previous, current, -3.0);
    const PaintSamples above = interpolate_paint_samples(previous, current, 7.0);
    const PaintSamples broken =
        interpolate_paint_samples(previous, current, std::numeric_limits<double>::quiet_NaN());
    ok = expect(near(*below.at(2).opacity, 0.0f) && near(*above.at(2).opacity, 1.0f) &&
                    near(*broken.at(2).opacity, 0.0f),
                "alpha outside [0,1] and NaN clamp safely") &&
         ok;

    // A node that only appears on the current tick has nothing to blend from and keeps its value.
    Node later(3);
    later.set_bounds({0, 0, 10, 10});
    PaintSamples with_new = current;
    for (const auto& [node, sample] : sample_paint(later, {{"opacity", "0.75"}})) {
        with_new[node] = sample;
    }
    const PaintSamples grown = interpolate_paint_samples(previous, with_new, 0.0);
    ok = expect(grown.size() == 2 && near(*grown.at(3).opacity, 0.75f),
                "a first-tick sample keeps its current value") &&
         ok;

    // A property that stopped being sampled is dropped rather than held forever.
    PaintSamples stale = current;
    stale.erase(2);
    const PaintSamples shrunk = interpolate_paint_samples(previous, stale, 0.5);
    ok = expect(shrunk.empty(), "a node missing from the current tick is dropped") && ok;
    return ok;
}

bool interpolation_blends_transform_components() {
    Node box(2);
    box.set_bounds({0, 0, 100, 50});
    const auto blend = [&](const char* from, const char* to) {
        const PaintSamples previous = sample_paint(box, {{"transform", from}});
        const PaintSamples current = sample_paint(box, {{"transform", to}});
        return std::pair(previous, current);
    };

    auto [scale_previous, scale_current] = blend("scale(1)", "scale(3)");
    const PaintSamples scaled = interpolate_paint_samples(scale_previous, scale_current, 0.25);
    bool ok = expect(scaled.size() == 1 && near(scaled.at(2).transform->m[0], 1.5f) &&
                         near(scaled.at(2).transform->m[4], 1.5f),
                     "scale blends linearly");

    auto [rotate_previous, rotate_current] = blend("rotate(0deg)", "rotate(90deg)");
    const PaintSamples rotated = interpolate_paint_samples(rotate_previous, rotate_current, 0.5);
    const float diagonal = std::sqrt(0.5f);
    ok = expect(near(rotated.at(2).transform->m[0], diagonal) &&
                    near(rotated.at(2).transform->m[1], -diagonal) &&
                    near(rotated.at(2).transform->m[3], diagonal) &&
                    near(rotated.at(2).transform->m[4], diagonal),
                "rotate blends along the angle") &&
         ok;

    // 170deg -> -170deg is +20deg, not -340deg: the shorter arc keeps the visual motion small.
    auto [wrap_previous, wrap_current] = blend("rotate(170deg)", "rotate(-170deg)");
    const PaintSamples wrapped = interpolate_paint_samples(wrap_previous, wrap_current, 0.5);
    ok = expect(near(wrapped.at(2).transform->m[0], -1.0f, 1.0e-4f) &&
                    near(wrapped.at(2).transform->m[3], 0.0f, 1.0e-4f),
                "rotation takes the shorter arc") &&
         ok;

    // A skewed matrix has no translate/scale/rotate form: it holds the current tick instead of
    // interpolating through a wrong shape.
    auto [skew_previous, skew_current] = blend("rotate(0deg)", "matrix(1, 0.5, 0, 1, 0, 0)");
    const PaintSamples skewed = interpolate_paint_samples(skew_previous, skew_current, 0.5);
    ok = expect(skewed.size() == 1 &&
                    equal_matrix(*skewed.at(2).transform, *skew_current.at(2).transform),
                "a skewed transform is held discrete") &&
         ok;

    // Alpha 1 reproduces the current tick exactly, and alpha 0 the previous one.
    auto [plain_previous, plain_current] = blend("translate(4px, 8px)", "translate(40px, 80px)");
    const PaintSamples start = interpolate_paint_samples(plain_previous, plain_current, 0.0);
    const PaintSamples end = interpolate_paint_samples(plain_previous, plain_current, 1.0);
    ok = expect(equal_matrix(*start.at(2).transform, *plain_previous.at(2).transform) &&
                    equal_matrix(*end.at(2).transform, *plain_current.at(2).transform),
                "alpha endpoints reproduce the tick samples exactly") &&
         ok;
    return ok;
}

bool interpolation_is_deterministic() {
    Node box(2);
    box.set_bounds({0, 0, 100, 50});
    const PaintSamples previous = sample_paint(
        box, {{"opacity", "0.2"}, {"background-color", "#102030"}, {"transform", "rotate(10deg)"}});
    const PaintSamples current = sample_paint(
        box, {{"opacity", "0.9"}, {"background-color", "#f0e0d0"}, {"transform", "rotate(80deg)"}});
    const PaintSamples first = interpolate_paint_samples(previous, current, 0.37);
    const PaintSamples second = interpolate_paint_samples(previous, current, 0.37);
    bool ok = expect(first.size() == second.size(), "deterministic sample size");
    for (const auto& [node, sample] : first) {
        const auto found = second.find(node);
        ok = expect(found != second.end() && near(*sample.opacity, *found->second.opacity) &&
                        equal_matrix(*sample.transform, *found->second.transform) &&
                        near_color(*sample.background_color, *found->second.background_color),
                    "repeated interpolation is bit-identical") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = sync_runs_only_on_style_or_layout_work_and_reaches_damage() && ok;
    ok = forced_sync_runs_on_a_quiet_tick() && ok;
    ok = capture_keeps_only_paint_visible_properties() && ok;
    ok = interpolation_blends_values_and_colors() && ok;
    ok = interpolation_blends_transform_components() && ok;
    ok = interpolation_is_deterministic() && ok;
    if (!ok) {
        std::cerr << "frame_pipeline_style_sync: FAILED\n";
        return 1;
    }
    std::cout << "frame_pipeline_style_sync: ok\n";
    return 0;
}
