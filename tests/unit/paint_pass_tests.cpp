// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::create_null_device;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::SceneStyleProvider;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_pass: " << message << "\n";
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

struct RecordedOp {
    enum class Kind { Rect, Rounded };

    Kind kind = Kind::Rect;
    Rect rect;
    Color color;
    float radius = 0.0f;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        ops.push_back({RecordedOp::Kind::Rect, rect, color, 0.0f});
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float radius) override {
        ops.push_back({RecordedOp::Kind::Rounded, rect, color, radius});
    }

    std::vector<RecordedOp> ops;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

bool same_color(const Color& actual, const Color& expected, float epsilon = 0.0f) {
    return std::abs(actual.r - expected.r) <= epsilon &&
           std::abs(actual.g - expected.g) <= epsilon &&
           std::abs(actual.b - expected.b) <= epsilon && std::abs(actual.a - expected.a) <= epsilon;
}

bool test_parent_offset_and_child_order() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});

    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10, 20, 30, 40});
    Node& child = root.add_child(std::move(child_owner));

    auto grandchild_owner = std::make_unique<Node>(3);
    grandchild_owner->set_bounds({5, 5, 10, 10});
    child.add_child(std::move(grandchild_owner));

    StubStyles styles;
    styles.set(2, styled({{"background-color", "red"}}));
    styles.set(3, styled({{"background-color", "blue"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(stats.nodes == 3, "three visible nodes") &&
           expect(stats.backgrounds == 2, "two backgrounds") &&
           expect(sink.ops.size() == 2, "parent inputs paint before children") &&
           expect(same_rect(sink.ops[0].rect, {10, 20, 30, 40}), "child rect is absolute") &&
           expect(same_color(sink.ops[0].color, tessera::rendering::rhi::rgba8(255, 0, 0)),
                  "child background color") &&
           expect(same_rect(sink.ops[1].rect, {15, 25, 10, 10}), "grandchild adds parent offset") &&
           expect(same_color(sink.ops[1].color, tessera::rendering::rhi::rgba8(0, 0, 255)),
                  "grandchild background color");
}

bool test_invisible_subtree_skipped() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});

    auto hidden_owner = std::make_unique<Node>(2);
    hidden_owner->set_bounds({10, 10, 50, 50});
    hidden_owner->set_visible(false);
    Node& hidden = root.add_child(std::move(hidden_owner));

    auto grandchild_owner = std::make_unique<Node>(3);
    grandchild_owner->set_bounds({0, 0, 10, 10});
    hidden.add_child(std::move(grandchild_owner));

    StubStyles styles;
    styles.set(2, styled({{"background-color", "red"}}));
    styles.set(3, styled({{"background-color", "blue"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(sink.ops.empty(), "hidden subtree emits nothing") &&
           expect(stats.nodes == 1, "only the root is visited") &&
           expect(stats.hidden_subtrees == 1, "hidden root is counted");
}

bool test_transparent_and_invalid_colors_skipped() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});

    auto transparent_owner = std::make_unique<Node>(2);
    transparent_owner->set_bounds({0, 0, 10, 10});
    root.add_child(std::move(transparent_owner));

    auto zero_alpha_owner = std::make_unique<Node>(3);
    zero_alpha_owner->set_bounds({10, 0, 10, 10});
    root.add_child(std::move(zero_alpha_owner));

    auto invalid_owner = std::make_unique<Node>(4);
    invalid_owner->set_bounds({20, 0, 10, 10});
    root.add_child(std::move(invalid_owner));

    StubStyles styles;
    styles.set(2, styled({{"background-color", "transparent"}}));
    styles.set(3, styled({{"background-color", "rgba(255, 0, 0, 0)"}}));
    styles.set(4, styled({{"background-color", "not-a-color"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(sink.ops.empty(), "transparent/invalid backgrounds are skipped") &&
           expect(stats.backgrounds == 0, "no backgrounds counted") &&
           expect(stats.nodes == 4, "all nodes visited");
}

bool test_border_edges() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});

    StubStyles styles;
    styles.set(1, styled({{"border-top-width", "1px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "red"},
                          {"border-right-width", "2px"},
                          {"border-right-style", "dashed"},
                          {"border-right-color", "green"},
                          {"border-bottom-width", "3px"},
                          {"border-bottom-style", "solid"},
                          {"border-bottom-color", "blue"},
                          {"border-left-width", "4px"},
                          {"border-left-style", "solid"},
                          {"border-left-color", "black"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(stats.borders == 4, "four border edges") &&
           expect(sink.ops.size() == 4, "one quad per edge") &&
           expect(same_rect(sink.ops[0].rect, {0, 0, 100, 1}), "top edge") &&
           expect(same_color(sink.ops[0].color, tessera::rendering::rhi::rgba8(255, 0, 0)),
                  "top color") &&
           expect(same_rect(sink.ops[1].rect, {98, 1, 2, 96}), "right edge") &&
           expect(same_color(sink.ops[1].color, tessera::rendering::rhi::rgba8(0, 128, 0)),
                  "right color") &&
           expect(same_rect(sink.ops[2].rect, {0, 97, 100, 3}), "bottom edge") &&
           expect(same_color(sink.ops[2].color, tessera::rendering::rhi::rgba8(0, 0, 255)),
                  "bottom color") &&
           expect(same_rect(sink.ops[3].rect, {0, 1, 4, 96}), "left edge") &&
           expect(same_color(sink.ops[3].color, tessera::rendering::rhi::rgba8(0, 0, 0)),
                  "left color");
}

bool test_border_none_is_not_painted() {
    Node root(1);
    root.set_bounds({0, 0, 20, 20});

    StubStyles styles;
    styles.set(1, styled({{"border-top-width", "4px"},
                          {"border-top-style", "none"},
                          {"border-top-color", "red"},
                          {"border-left-width", "4px"},
                          {"border-left-style", "hidden"},
                          {"border-left-color", "blue"}}));

    RecordingSink sink;
    paint_scene(root, styles, sink);
    return expect(sink.ops.empty(), "none/hidden border sides are skipped");
}

bool test_rounded_radius_and_clamp() {
    const auto radii = [](const char* radius) {
        return styled({{"background-color", "red"},
                       {"border-top-left-radius", radius},
                       {"border-top-right-radius", radius},
                       {"border-bottom-right-radius", radius},
                       {"border-bottom-left-radius", radius}});
    };

    RecordingSink uniform_sink;
    Node uniform_root(1);
    uniform_root.set_bounds({0, 0, 100, 100});
    StubStyles uniform_styles;
    uniform_styles.set(1, radii("8px"));
    paint_scene(uniform_root, uniform_styles, uniform_sink);

    RecordingSink clamped_sink;
    Node clamped_root(1);
    clamped_root.set_bounds({0, 0, 100, 100});
    StubStyles clamped_styles;
    clamped_styles.set(1, radii("200px"));
    paint_scene(clamped_root, clamped_styles, clamped_sink);

    RecordingSink mixed_sink;
    Node mixed_root(1);
    mixed_root.set_bounds({0, 0, 100, 100});
    StubStyles mixed_styles;
    mixed_styles.set(1, radii("8px"));
    // One different corner disables the single rounded rect.
    ComputedStyle mixed_style = radii("8px");
    mixed_style.set("border-bottom-left-radius", "0");
    mixed_styles.set(1, mixed_style);
    paint_scene(mixed_root, mixed_styles, mixed_sink);

    return expect(uniform_sink.ops.size() == 1, "one rounded background") &&
           expect(uniform_sink.ops[0].kind == RecordedOp::Kind::Rounded, "rounded op kind") &&
           expect(uniform_sink.ops[0].radius == 8.0f, "radius is preserved") &&
           expect(same_rect(uniform_sink.ops[0].rect, {0, 0, 100, 100}), "rounded rect bounds") &&
           expect(clamped_sink.ops.size() == 1 && clamped_sink.ops[0].radius == 50.0f,
                  "radius clamps to half the box") &&
           expect(mixed_sink.ops.size() == 1 && mixed_sink.ops[0].kind == RecordedOp::Kind::Rect,
                  "non-uniform radii fall back to a plain rect");
}

bool test_opacity_and_current_color() {
    Node root(1);
    root.set_bounds({0, 0, 40, 40});

    StubStyles styles;
    styles.set(1, styled({{"background-color", "rgba(255, 0, 0, 0.5)"},
                          {"opacity", "0.5"},
                          {"color", "blue"},
                          {"border-top-width", "2px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "currentColor"}}));

    RecordingSink sink;
    paint_scene(root, styles, sink);

    const Color background{1.0f, 0.0f, 0.0f, 0.25f};
    const Color border{0.0f, 0.0f, 1.0f, 0.5f};
    return expect(sink.ops.size() == 2, "background and top border") &&
           expect(same_color(sink.ops[0].color, background),
                  "opacity multiplies background alpha") &&
           expect(same_color(sink.ops[1].color, border), "currentColor + opacity on the border");
}

bool test_empty_scene() {
    Node root(1);
    root.set_bounds({0, 0, 0, 0});

    StubStyles styles;
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(sink.ops.empty(), "zero-size node paints nothing") &&
           expect(stats.nodes == 1, "root still visited") &&
           expect(stats.backgrounds == 0 && stats.borders == 0, "no paint counters");
}

bool test_scene_style_provider() {
    Node root(1);
    root.set_bounds({0, 0, 10, 10});

    const auto sheet = compile_stylesheet("* { background-color: red; }");
    SceneStyleTree tree(root);
    SceneStyleProvider styles(sheet, tree);

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(stats.backgrounds == 1, "stylesheet background is painted") &&
           expect(sink.ops.size() == 1 && same_rect(sink.ops[0].rect, {0, 0, 10, 10}),
                  "computed style drives the rect") &&
           expect(same_color(sink.ops[0].color, tessera::rendering::rhi::rgba8(255, 0, 0)),
                  "computed style color");
}

bool test_null_backend_integration() {
    auto device = create_null_device({});
    auto target = device->create_render_target({{64, 64}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    auto commands = device->create_command_list();

    Batched2DRenderer renderer(*device, *pipeline, *white);

    Node root(1);
    root.set_bounds({0, 0, 64, 64});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({8, 8, 16, 16});
    root.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(1, styled({{"background-color", "red"}}));
    styles.set(2, styled({{"background-color", "blue"}}));

    renderer.begin(*commands, *target, {{64, 64}});
    Batched2DSink sink(renderer);
    const PaintStats stats = paint_scene(root, styles, sink);
    renderer.end();

    auto* null_commands = dynamic_cast<NullCommandList*>(commands.get());
    // Both backgrounds share the (scissor, texture) batch key, so the renderer emits one draw
    // of two quads (12 vertices).
    bool ok =
        expect(null_commands != nullptr, "null command list type") &&
        expect(stats.backgrounds == 2, "two backgrounds painted") &&
        expect(renderer.draw_calls() == 1, "one batched draw call") &&
        expect(renderer.vertex_count() == 12, "two quads batched into one draw") &&
        expect(null_commands->stats().draws == 1, "null backend recorded the draw") &&
        expect(null_commands->stats().vertices == 12, "null backend vertex count") &&
        expect(null_commands->stats().draw_records.size() == 1, "one draw record for the batch") &&
        expect(!null_commands->submitted(), "paint does not submit");

    commands->submit();
    ok = ok && expect(null_commands->submitted(), "caller submits explicitly");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_parent_offset_and_child_order() && ok;
    ok = test_invisible_subtree_skipped() && ok;
    ok = test_transparent_and_invalid_colors_skipped() && ok;
    ok = test_border_edges() && ok;
    ok = test_border_none_is_not_painted() && ok;
    ok = test_rounded_radius_and_clamp() && ok;
    ok = test_opacity_and_current_color() && ok;
    ok = test_empty_scene() && ok;
    ok = test_scene_style_provider() && ok;
    ok = test_null_backend_integration() && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "paint_pass: ok\n";
    return 0;
}
