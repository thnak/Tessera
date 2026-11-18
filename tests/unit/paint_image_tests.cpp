// SPDX-License-Identifier: MIT
//
// PaintData image contract: precedence among text/vector/image, emission order (after
// background/borders, inside the node clip, before children), content box/padding, tint/opacity
// folding, size/scale passthrough, skip cases, damage-region replay, and the S8 sampling-filter
// decision table (nearest at aligned 1:1, linear otherwise).

#include "image_filter.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::ImageDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_image: " << message << "\n";
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
    enum class Kind { Rect, Image, ClipPush, ClipPop };

    Kind kind = Kind::Rect;
    rhi::Rect rect;
    ImageDraw image;
    std::size_t clip_depth = 0;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const rhi::Rect& rect, const rhi::Color& /*color*/) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Rect;
        op.rect = rect;
        op.clip_depth = clip_depth_;
        ops.push_back(op);
    }

    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        fill_rect(rect, color);
        ops.back().kind = RecordedOp::Kind::Rect;
        (void)radius;
    }

    void push_clip(const tessera::ui::paint::RoundedClip& clip) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::ClipPush;
        op.rect = clip.rect;
        op.clip_depth = ++clip_depth_;
        ops.push_back(op);
    }

    void pop_clip() override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::ClipPop;
        op.clip_depth = --clip_depth_;
        ops.push_back(op);
    }

    void draw_image(const ImageDraw& draw) override {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Image;
        op.rect = draw.rect;
        op.image = draw;
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
    std::size_t clip_depth_ = 0;
};

[[nodiscard]] bool same_rect(const rhi::Rect& actual, const rhi::Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

[[nodiscard]] bool near(float actual, float expected, float epsilon = 1.0e-4f) {
    return std::fabs(actual - expected) <= epsilon;
}

[[nodiscard]] PaintData image_paint(std::string_view asset) {
    PaintData data;
    data.kind = PaintKind::Image;
    data.image.asset = asset;
    return data;
}

bool emission_order_and_payload() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
    Node& window = root.add_child(std::make_unique<Node>(2));
    window.set_bounds({10.0f, 20.0f, 120.0f, 80.0f});
    PaintData data = image_paint("images/checker.png");
    data.image.raster_width = 96;
    data.image.raster_height = 48;
    data.image.scale = 2.0f;
    data.image.tint = rhi::Color{1.0f, 0.5f, 0.25f, 0.5f};
    data.image.opacity = 0.5f;
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
    bool ok = expect(stats.images == 1, "one image draw is emitted") &&
              expect(sink.count(RecordedOp::Kind::Image) == 1, "sink saw one image draw");
    std::vector<RecordedOp::Kind> order;
    for (const RecordedOp& op : sink.ops) {
        order.push_back(op.kind);
    }
    ok = expect(order.size() == 4 && order[0] == RecordedOp::Kind::Rect &&
                    order[1] == RecordedOp::Kind::Rect && order[2] == RecordedOp::Kind::Image &&
                    order[3] == RecordedOp::Kind::Rect,
                "the image paints after its background and before the children") &&
         ok;
    const RecordedOp& drawn = sink.ops[2];
    ok = expect(same_rect(drawn.rect, rhi::Rect{16.0f, 28.0f, 104.0f, 68.0f}),
                "the image rect is the padded content box") &&
         expect(drawn.image.asset == "images/checker.png", "the asset name is passed through") &&
         expect(drawn.image.raster_width == 96 && drawn.image.raster_height == 48,
                "the explicit raster size is passed through") &&
         expect(near(drawn.image.scale, 2.0f), "the scale hint is passed through") &&
         expect(near(drawn.image.tint.r, 1.0f) && near(drawn.image.tint.g, 0.5f) &&
                    near(drawn.image.tint.b, 0.25f) && near(drawn.image.tint.a, 0.125f),
                "tint alpha folds tint, payload opacity and node opacity") &&
         ok;
    return ok;
}

bool precedence_one_kind() {
    Node node(1);
    node.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}}));

    PaintData image_kind = image_paint("images/checker.png");
    image_kind.text = "ignored";
    image_kind.vector.asset = "icons/ignored.svg";
    node.set_paint_data(std::move(image_kind));
    RecordingSink image_sink;
    paint_scene(node, styles, image_sink);
    bool ok = expect(image_sink.count(RecordedOp::Kind::Image) == 1,
                     "kind Image paints the image payload");

    PaintData vector_kind;
    vector_kind.kind = PaintKind::Vector;
    vector_kind.vector.asset = "icons/a.svg";
    vector_kind.image.asset = "images/checker.png";
    node.set_paint_data(std::move(vector_kind));
    RecordingSink vector_sink;
    paint_scene(node, styles, vector_sink);
    ok = expect(vector_sink.count(RecordedOp::Kind::Image) == 0,
                "a vector node ignores the image payload") &&
         ok;
    return ok;
}

bool clipping_and_skips() {
    Node parent(1);
    parent.set_bounds({0.0f, 0.0f, 120.0f, 120.0f});
    Node& child = parent.add_child(std::make_unique<Node>(2));
    child.set_bounds({10.0f, 10.0f, 80.0f, 80.0f});
    child.set_paint_data(image_paint("images/clipped.png"));
    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"}, {"overflow-y", "hidden"}}));
    RecordingSink sink;
    paint_scene(parent, styles, sink);
    bool ok = expect(sink.count(RecordedOp::Kind::ClipPush) == 1, "the clip is pushed once");
    for (const RecordedOp& op : sink.ops) {
        if (op.kind == RecordedOp::Kind::Image) {
            ok = expect(op.clip_depth == 1, "the image is emitted inside the active clip") && ok;
        }
    }

    const auto no_image = [](PaintData data, ComputedStyle style) {
        Node node(7);
        node.set_bounds({0.0f, 0.0f, 40.0f, 40.0f});
        node.set_paint_data(std::move(data));
        StubStyles node_styles;
        node_styles.set(7, std::move(style));
        RecordingSink node_sink;
        paint_scene(node, node_styles, node_sink);
        return node_sink.count(RecordedOp::Kind::Image) == 0;
    };
    ok =
        expect(no_image(image_paint(""), ComputedStyle{}), "empty asset name paints nothing") && ok;
    const std::string long_name(PaintData::kMaxAssetBytes + 1, 'a');
    ok = expect(no_image(image_paint(long_name), ComputedStyle{}),
                "over-long asset names are ignored") &&
         ok;
    PaintData zero_opacity = image_paint("images/checker.png");
    zero_opacity.image.opacity = 0.0f;
    ok = expect(no_image(std::move(zero_opacity), ComputedStyle{}),
                "zero payload opacity paints nothing") &&
         ok;
    ok = expect(no_image(image_paint("images/checker.png"),
                         styled({{"padding-left", "20px"}, {"padding-right", "20px"}})),
                "a consumed content box paints nothing") &&
         ok;
    return ok;
}

bool damage_replay() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
    root.set_paint_data(image_paint("images/damaged.png"));
    StubStyles styles;

    RecordingSink target;
    {
        std::vector<rhi::Scissor> regions = {rhi::Scissor{0, 0, 50, 50}};
        ClippedPaintSink clipped(target, regions);
        paint_scene(root, styles, clipped);
    }
    bool ok = expect(target.count(RecordedOp::Kind::Image) == 1,
                     "an image intersecting one damage region replays once");
    for (const RecordedOp& op : target.ops) {
        if (op.kind == RecordedOp::Kind::Image) {
            ok = expect(op.clip_depth == 1, "the replay runs under the region clip") && ok;
        }
    }
    RecordingSink outside;
    {
        std::vector<rhi::Scissor> regions = {rhi::Scissor{300, 300, 50, 50}};
        ClippedPaintSink clipped(outside, regions);
        paint_scene(root, styles, clipped);
    }
    ok = expect(outside.count(RecordedOp::Kind::Image) == 0,
                "an image outside every region is not replayed") &&
         ok;
    RecordingSink twice;
    {
        std::vector<rhi::Scissor> regions = {rhi::Scissor{0, 0, 50, 50},
                                             rhi::Scissor{100, 100, 50, 50}};
        ClippedPaintSink clipped(twice, regions);
        paint_scene(root, styles, clipped);
    }
    ok = expect(twice.count(RecordedOp::Kind::Image) == 2,
                "two intersecting regions replay the image twice") &&
         ok;
    return ok;
}

// S8 filter-selection table (docs/design/display-aware-rendering.md section 4). The decision is
// geometric: nearest only when the drawn device extent equals the uploaded raster extent within
// half a device pixel on both axes AND the device-space origin is a whole pixel.
//
// Note what the raster-size rule means for the "fractional scale" cases: an image is decoded at
// display size x DPR (issue #14), so at device scale 1.25 an integer CSS box whose device origin is
// whole has a raster extent exactly as large as the drawn extent, and nearest is exact. A
// fractional scale flips to linear when it lands on a fractional device origin, which is the common
// case for non-multiple-of-four CSS geometry, or when an explicit raster extent disagrees with the
// draw.
// RHI 1.6 (#491): a draw that minifies the raster by 2x or more on either axis asks for the full
// chain, and only when the device can build one.
bool mip_level_selection_table() {
    using tessera::ui::paint::image_levels_for_draw;

    const rhi::Extent2D raster{128, 64};
    const auto levels = [&](float width, float height, bool supported = true) {
        return image_levels_for_draw(rhi::Rect{0.0f, 0.0f, width, height}, raster, supported);
    };
    bool ok = expect(levels(128.0f, 64.0f) == 1, "1:1 asks for a single level");
    ok = expect(levels(256.0f, 128.0f) == 1, "magnification asks for a single level") && ok;
    ok = expect(levels(100.0f, 50.0f) == 1, "a sub-2x downscale asks for a single level") && ok;
    ok = expect(levels(64.0f, 64.0f) == rhi::full_mip_levels(raster),
                "a 2x downscale on one axis asks for the full chain (128 -> 8 levels)") &&
         ok;
    ok = expect(levels(32.0f, 16.0f) == 8, "a 4x downscale asks for the full chain") && ok;
    ok = expect(levels(32.0f, 16.0f, false) == 1,
                "a device without mip support always gets a single level") &&
         ok;
    ok = expect(levels(0.0f, 16.0f) == 1 && levels(-8.0f, 16.0f) == 1,
                "a degenerate rect asks for a single level") &&
         ok;
    ok = expect(image_levels_for_draw(rhi::Rect{0.0f, 0.0f, 32.0f, 32.0f}, rhi::Extent2D{0, 64},
                                      true) == 1,
                "a zero raster extent asks for a single level") &&
         ok;
    ok = expect(image_levels_for_draw(
                    rhi::Rect{0.0f, 0.0f, std::numeric_limits<float>::infinity(), 16.0f}, raster,
                    true) == 1,
                "a non-finite rect asks for a single level") &&
         ok;
    return ok;
}

bool filter_selection_table() {
    using tessera::ui::paint::image_filter_for_draw;

    const auto decide = [](float css_x, float css_y, float css_w, float css_h, float scale,
                           std::uint32_t raster_w, std::uint32_t raster_h) {
        const rhi::Rect device{css_x * scale, css_y * scale, css_w * scale, css_h * scale};
        return image_filter_for_draw(device, rhi::Extent2D{raster_w, raster_h}, device.x, device.y);
    };

    // Exact 1:1 integer geometry: the CpuCanvas/parity case, the previous fixed filter.
    bool ok = expect(decide(8.0f, 8.0f, 64.0f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Nearest,
                     "aligned 1:1 integer positions sample nearest");
    ok = expect(decide(0.0f, 0.0f, 10.0f, 10.0f, 1.0f, 10, 10) == rhi::Filter::Nearest,
                "a whole-pixel origin at 0 samples nearest") &&
         ok;

    // Fractional device scale. 9 * 1.25 = 11.25 (fractional origin); 1 * 0.5 = 0.5.
    ok = expect(decide(9.0f, 0.0f, 10.0f, 10.0f, 1.25f, 13, 13) == rhi::Filter::Linear,
                "a fractional scale on a fractional device origin samples linear") &&
         ok;
    ok = expect(decide(1.0f, 0.0f, 64.0f, 64.0f, 0.5f, 32, 32) == rhi::Filter::Linear,
                "a half scale on a half-pixel origin samples linear") &&
         ok;
    // The raster is decoded at display size, so 8 * 1.25 = 10 (whole) and 64 * 1.25 == 80 == the
    // raster extent: this draw really is 1:1 with its texture, fractional device scale or not.
    ok = expect(decide(8.0f, 0.0f, 64.0f, 64.0f, 1.25f, 80, 80) == rhi::Filter::Nearest,
                "a fractional scale on a whole origin and matching raster stays nearest") &&
         ok;

    // 1:1 extent with a fractional origin.
    ok = expect(decide(8.5f, 8.0f, 64.0f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Linear,
                "a 1:1 extent with a fractional x origin samples linear") &&
         ok;
    ok = expect(decide(8.0f, 8.25f, 64.0f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Linear,
                "a 1:1 extent with a fractional y origin samples linear") &&
         ok;

    // Downscaled draw: the draw covers fewer device pixels than the texture has (the design's mip
    // case; without mips it is plain linear minification).
    ok = expect(decide(0.0f, 0.0f, 64.0f, 64.0f, 1.0f, 128, 128) == rhi::Filter::Linear,
                "a draw smaller than its raster samples linear") &&
         ok;
    // Stretched on one axis only.
    ok = expect(decide(0.0f, 0.0f, 64.0f, 64.0f, 1.0f, 64, 32) == rhi::Filter::Linear,
                "an aspect-stretched draw samples linear") &&
         ok;

    // The extent tolerance is exactly half a device pixel (rounding can never exceed it).
    ok = expect(decide(0.0f, 0.0f, 64.0f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Nearest,
                "an exact extent samples nearest") &&
         ok;
    ok = expect(decide(0.0f, 0.0f, 64.5f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Nearest,
                "half a device pixel past the raster still samples nearest") &&
         ok;
    ok = expect(decide(0.0f, 0.0f, 64.6f, 64.0f, 1.0f, 64, 64) == rhi::Filter::Linear,
                "beyond half a device pixel samples linear") &&
         ok;

    // Degenerate requests: the paint pass skips them, so the deterministic default is the answer.
    ok = expect(image_filter_for_draw(rhi::Rect{}, rhi::Extent2D{64, 64}, 0.0f, 0.0f) ==
                    rhi::Filter::Nearest,
                "a zero-extent rect takes the safe default") &&
         ok;
    ok = expect(image_filter_for_draw(rhi::Rect{0.0f, 0.0f, 64.0f, 64.0f}, rhi::Extent2D{0, 64},
                                      0.0f, 0.0f) == rhi::Filter::Nearest,
                "a zero raster extent takes the safe default") &&
         ok;
    ok = expect(image_filter_for_draw(rhi::Rect{0.0f, 0.0f, -64.0f, 64.0f}, rhi::Extent2D{64, 64},
                                      0.0f, 0.0f) == rhi::Filter::Nearest,
                "a negative extent takes the safe default") &&
         ok;
    ok = expect(image_filter_for_draw(
                    rhi::Rect{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 64.0f},
                    rhi::Extent2D{64, 64}, 0.0f, 0.0f) == rhi::Filter::Nearest,
                "a non-finite extent takes the safe default") &&
         ok;

    return ok;
}

bool decode_target_follows_the_draw() {
    ImageDraw draw;
    draw.rect = {0.0f, 0.0f, 200.0f, 150.0f};
    // 200x150 logical on a 3x screen needs 600x450 pixels: buckets 768 and 512.
    auto target = tessera::ui::paint::image_decode_target(draw, 3.0f);
    bool ok = expect(target.width == 768 && target.height == 512, "content box x device scale");
    draw.scale = 1.0f;
    target = tessera::ui::paint::image_decode_target(draw, 3.0f);
    ok = expect(target.width == 256 && target.height == 192, "an explicit scale wins") && ok;
    draw.raster_width = 100;
    draw.raster_height = 100;
    target = tessera::ui::paint::image_decode_target(draw, 3.0f);
    ok = expect(target.width == 128 && target.height == 128, "explicit extents win") && ok;
    draw.raster_height = 0;
    target = tessera::ui::paint::image_decode_target(draw, 3.0f);
    ok = expect(target.width == target.height && target.width >= 192,
                "one explicit extent keeps a square box at least as large as needed") &&
         ok;
    draw.decode_original = true;
    target = tessera::ui::paint::image_decode_target(draw, 3.0f);
    return expect(target.original(), "decodeSize=original decodes at source size") && ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = emission_order_and_payload() && ok;
    ok = precedence_one_kind() && ok;
    ok = clipping_and_skips() && ok;
    ok = damage_replay() && ok;
    ok = filter_selection_table() && ok;
    ok = mip_level_selection_table() && ok;
    ok = decode_target_follows_the_draw() && ok;
    if (!ok) {
        std::cerr << "paint_image: FAILED\n";
        return 1;
    }
    std::cout << "paint_image: ok\n";
    return 0;
}
