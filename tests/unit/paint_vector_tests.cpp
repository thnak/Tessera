// SPDX-License-Identifier: MIT
//
// Vector PaintData contract: precedence between text and vector payloads, emission order (after
// background/borders, inside the node clip, before children), content box/padding, tint and opacity
// folding, size/scale hints, damage-clipped replay, and the documented no-op behavior of
// (the Batched2DSink no-op/scale/raster contracts live in paint_vector_sink_tests.cpp).

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace assets = tessera::ui::assets;
namespace path = tessera::rendering::path;

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::create_null_device;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::test::path_texture::FakeDevice;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintVectorStyle;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::VectorDraw;
using tessera::ui::style::ComputedStyle;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_vector: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

[[nodiscard]] ComputedStyle
styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

struct RecordedOp {
    enum class Kind { Rect, Rounded, Vector, Text, ClipPush, ClipPop };

    Kind kind = Kind::Rect;
    Rect rect;
    Color color;
    float radius = 0.0f;
    VectorDraw vector;
    std::size_t clip_depth = 0;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Rect;
        op.rect = rect;
        op.color = color;
        op.clip_depth = clip_depth_;
        ops.push_back(op);
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float radius) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Rounded;
        op.rect = rect;
        op.color = color;
        op.radius = radius;
        op.clip_depth = clip_depth_;
        ops.push_back(op);
    }

    void push_clip(const tessera::ui::paint::RoundedClip& clip) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::ClipPush;
        op.rect = clip.rect;
        op.clip_depth = ++clip_depth_;
        clips.push_back(clip);
        ops.push_back(op);
    }

    void pop_clip() override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::ClipPop;
        op.clip_depth = --clip_depth_;
        ops.push_back(op);
    }

    void draw_text(const tessera::ui::paint::TextDraw& run) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Text;
        op.rect = run.rect;
        op.clip_depth = clip_depth_;
        ops.push_back(op);
    }

    void draw_vector(const VectorDraw& draw) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Vector;
        op.vector = draw;
        op.rect = draw.rect;
        op.clip_depth = clip_depth_;
        ops.push_back(op);
    }

    [[nodiscard]] std::size_t count(RecordedOp::Kind kind) const {
        std::size_t total = 0;
        for (const RecordedOp& op : ops) {
            if (op.kind == kind) {
                ++total;
            }
        }
        return total;
    }

    std::vector<RecordedOp> ops;
    std::vector<tessera::ui::paint::RoundedClip> clips;
    std::size_t clip_depth_ = 0;
};

[[nodiscard]] bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

[[nodiscard]] bool near(float actual, float expected, float epsilon = 1.0e-4f) {
    return std::fabs(actual - expected) <= epsilon;
}

[[nodiscard]] bool same_color(const Color& actual, const Color& expected, float epsilon = 1.0e-4f) {
    return near(actual.r, expected.r, epsilon) && near(actual.g, expected.g, epsilon) &&
           near(actual.b, expected.b, epsilon) && near(actual.a, expected.a, epsilon);
}

[[nodiscard]] PaintData vector_paint(std::string_view asset) {
    PaintData data;
    data.kind = PaintKind::Vector;
    data.vector.asset = asset;
    return data;
}

bool emission_order_and_payload() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
    Node& window = root.add_child(std::make_unique<Node>(2));
    window.set_bounds({10.0f, 20.0f, 120.0f, 80.0f});
    PaintData data = vector_paint("icons/icon.svg");
    data.vector.raster_width = 96;
    data.vector.raster_height = 48;
    data.vector.scale = 2.0f;
    data.vector.tint = Color{1.0f, 0.5f, 0.25f, 0.5f};
    data.vector.opacity = 0.5f;
    window.set_paint_data(std::move(data));
    Node& child = window.add_child(std::make_unique<Node>(3));
    child.set_bounds({0.0f, 0.0f, 20.0f, 20.0f});

    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"}}));
    styles.set(2, styled({{"background-color", "red"},
                          {"opacity", "0.5"},
                          {"padding-left", "6px"},
                          {"padding-right", "10px"},
                          {"padding-top", "8px"},
                          {"padding-bottom", "4px"}}));
    styles.set(3, styled({{"background-color", "blue"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    bool ok = expect(stats.vectors == 1, "one vector draw is emitted") &&
              expect(sink.count(RecordedOp::Kind::Vector) == 1, "sink saw one vector draw");

    // Order: root bg, window bg, vector, child bg.
    std::vector<RecordedOp::Kind> order;
    for (const RecordedOp& op : sink.ops) {
        order.push_back(op.kind);
    }
    ok = expect(order.size() == 4 && order[0] == RecordedOp::Kind::Rect &&
                    order[1] == RecordedOp::Kind::Rect && order[2] == RecordedOp::Kind::Vector &&
                    order[3] == RecordedOp::Kind::Rect,
                "vector paints after its background and before the children") &&
         ok;

    const RecordedOp& drawn = sink.ops[2];
    ok = expect(same_rect(drawn.rect, Rect{16.0f, 28.0f, 104.0f, 68.0f}),
                "vector rect is the padded content box") &&
         expect(drawn.vector.asset == "icons/icon.svg", "asset name is passed through") &&
         expect(drawn.vector.raster_width == 96 && drawn.vector.raster_height == 48,
                "explicit raster size is passed through") &&
         expect(near(drawn.vector.scale, 2.0f), "scale hint is passed through") &&
         expect(same_color(drawn.vector.tint, Color{1.0f, 0.5f, 0.25f, 0.125f}),
                "tint alpha folds tint, payload opacity and node opacity") &&
         ok;
    return ok;
}

bool precedence_one_kind() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});

    // kind Vector: text fields are ignored.
    PaintData vector_kind = vector_paint("icons/a.svg");
    vector_kind.text = "ignored";
    root.set_paint_data(std::move(vector_kind));
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));
    RecordingSink vector_sink;
    paint_scene(root, styles, vector_sink);
    bool ok = expect(vector_sink.count(RecordedOp::Kind::Vector) == 1 &&
                         vector_sink.count(RecordedOp::Kind::Text) == 0,
                     "kind Vector paints only the vector payload");

    // kind Text: the vector payload is ignored.
    PaintData text_kind;
    text_kind.kind = PaintKind::Text;
    text_kind.text = "hello";
    text_kind.vector.asset = "icons/a.svg";
    root.set_paint_data(std::move(text_kind));
    RecordingSink text_sink;
    paint_scene(root, styles, text_sink);
    ok = expect(text_sink.count(RecordedOp::Kind::Text) == 1 &&
                    text_sink.count(RecordedOp::Kind::Vector) == 0,
                "kind Text paints only the text payload") &&
         ok;
    return ok;
}

bool clipping_and_skips() {
    Node parent(1);
    parent.set_bounds({0.0f, 0.0f, 120.0f, 120.0f});
    Node& child = parent.add_child(std::make_unique<Node>(2));
    child.set_bounds({10.0f, 10.0f, 80.0f, 80.0f});
    child.set_paint_data(vector_paint("icons/clipped.svg"));

    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"}, {"overflow-y", "hidden"}}));
    styles.set(2, styled({{"padding-left", "0px"}}));
    RecordingSink sink;
    paint_scene(parent, styles, sink);
    bool ok = expect(sink.count(RecordedOp::Kind::ClipPush) == 1, "child clip is pushed once");
    for (const RecordedOp& op : sink.ops) {
        if (op.kind == RecordedOp::Kind::Vector) {
            ok = expect(op.clip_depth == 1, "vector is emitted inside the active clip") && ok;
        }
    }

    // Skips: empty name, over-long name, zero opacity, fully consumed content box.
    const auto no_vector = [](PaintData data, ComputedStyle style) {
        Node node(7);
        node.set_bounds({0.0f, 0.0f, 40.0f, 40.0f});
        node.set_paint_data(std::move(data));
        StubStyles node_styles;
        node_styles.set(7, std::move(style));
        RecordingSink node_sink;
        paint_scene(node, node_styles, node_sink);
        return node_sink.count(RecordedOp::Kind::Vector) == 0;
    };
    ok = expect(no_vector(vector_paint(""), ComputedStyle{}), "empty asset name paints nothing") &&
         ok;
    const std::string long_name(PaintData::kMaxAssetBytes + 1, 'a');
    ok = expect(no_vector(vector_paint(long_name), ComputedStyle{}),
                "over-long asset names are ignored, never truncated") &&
         ok;
    PaintData zero_opacity = vector_paint("icons/a.svg");
    zero_opacity.vector.opacity = 0.0f;
    ok = expect(no_vector(std::move(zero_opacity), ComputedStyle{}),
                "zero payload opacity paints nothing") &&
         ok;
    PaintData padded = vector_paint("icons/a.svg");
    ok = expect(no_vector(std::move(padded),
                          styled({{"padding-left", "20px"}, {"padding-right", "20px"}})),
                "a consumed content box paints nothing") &&
         ok;
    return ok;
}

bool damage_replay() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
    root.set_paint_data(vector_paint("icons/damaged.svg"));
    StubStyles styles;

    RecordingSink target;
    {
        std::vector<tessera::rendering::rhi::Scissor> regions = {
            tessera::rendering::rhi::Scissor{0, 0, 50, 50}};
        ClippedPaintSink clipped(target, regions);
        paint_scene(root, styles, clipped);
    }
    bool ok = expect(target.count(RecordedOp::Kind::Vector) == 1,
                     "a vector intersecting one damage region replays once");
    for (const RecordedOp& op : target.ops) {
        if (op.kind == RecordedOp::Kind::Vector) {
            ok = expect(op.clip_depth == 1, "the replay runs under the region clip") && ok;
        }
    }

    RecordingSink outside;
    {
        std::vector<tessera::rendering::rhi::Scissor> regions = {
            tessera::rendering::rhi::Scissor{300, 300, 50, 50}};
        ClippedPaintSink clipped(outside, regions);
        paint_scene(root, styles, clipped);
    }
    ok = expect(outside.count(RecordedOp::Kind::Vector) == 0,
                "a vector outside every damage region is not replayed") &&
         ok;

    RecordingSink twice;
    {
        std::vector<tessera::rendering::rhi::Scissor> regions = {
            tessera::rendering::rhi::Scissor{0, 0, 50, 50},
            tessera::rendering::rhi::Scissor{100, 100, 50, 50}};
        ClippedPaintSink clipped(twice, regions);
        paint_scene(root, styles, clipped);
    }
    ok = expect(twice.count(RecordedOp::Kind::Vector) == 2,
                "two intersecting damage regions replay the vector twice") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = emission_order_and_payload() && ok;
    ok = precedence_one_kind() && ok;
    ok = clipping_and_skips() && ok;
    ok = damage_replay() && ok;
    if (!ok) {
        std::cerr << "paint_vector: FAILED\n";
        return 1;
    }
    std::cout << "paint_vector: ok\n";
    return 0;
}
