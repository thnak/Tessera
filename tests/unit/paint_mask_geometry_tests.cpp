// SPDX-License-Identifier: MIT
//
// Mask geometry in the paint pass (issue #617): `mask-size`, `mask-position`, `mask-repeat` and
// `mask-mode` decide where each mask layer's gradient is drawn inside the node's border box. The
// stream is checked against hand-computed tiles: placement, repeat/space/round, the clip that cuts
// an overflowing tile to the box, per-layer cycling, the tile cap and luminance stops, and the
// `mask-origin` / `mask-clip` boxes (border, padding, content, no-clip).

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using namespace tessera::ui::paint;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_mask_geometry: " << message << "\n";
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

// Logs only the mask bracket: gradient boxes with the first and last stop alpha, and the clips.
class LogSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void fill_linear_gradient(const GradientDraw& draw) override {
        const auto n = [](float v) { return std::to_string(std::lround(v)); };
        log.push_back("grad(" + n(draw.rect.x) + "," + n(draw.rect.y) + "," + n(draw.rect.width) +
                      "," + n(draw.rect.height) + ")a" + n(draw.stops[0].color.a * 100.0f) + "-" +
                      n(draw.stops[draw.stop_count - 1].color.a * 100.0f));
    }
    void push_clip(const RoundedClip& clip) override {
        log.push_back("clip(" + std::to_string(std::lround(clip.rect.x)) + "," +
                      std::to_string(std::lround(clip.rect.y)) + "," +
                      std::to_string(std::lround(clip.rect.width)) + "," +
                      std::to_string(std::lround(clip.rect.height)) + ")");
    }
    void pop_clip() override {
        log.push_back("unclip");
    }
    [[nodiscard]] bool supports_layers() const override {
        return true;
    }
    void push_layer(const LayerDraw&) override {}
    void pop_layer() override {}
    [[nodiscard]] bool begin_layer_mask() override {
        return true;
    }
    void end_layer_mask() override {}
    [[nodiscard]] bool begin_mask_layer(tessera::rendering::gpu::MaskCompositeOp op) override {
        using Op = tessera::rendering::gpu::MaskCompositeOp;
        log.push_back(std::string("part(") +
                      (op == Op::Subtract    ? "subtract"
                       : op == Op::Intersect ? "intersect"
                                             : "exclude") +
                      ")");
        return !decline_parts;
    }
    void end_mask_layer() override {
        log.push_back("endpart");
    }
    bool decline_parts = false;
    std::vector<std::string> log;
};

using Log = std::vector<std::string>;

// The stats of the latest paint(), for the checks that read a counter.
PaintStats g_stats;

constexpr const char* kRamp = "linear-gradient(90deg, #000000, #ffffff)";

// One node at (10,10) 60x60 with the given mask properties (property/value pairs).
Log paint(std::initializer_list<std::pair<const char*, const char*>> mask) {
    Node root{1};
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    style.set("background-color", "#00ff00");
    for (const auto& [property, value] : mask) {
        style.set(property, value);
    }
    styles.set(2, style);
    LogSink sink;
    g_stats = paint_scene(root, styles, sink);
    return sink.log;
}

bool dump(const Log& log) {
    for (const auto& entry : log) {
        std::cerr << "  " << entry;
    }
    std::cerr << "\n";
    return false;
}

bool check(const char* message, const Log& got, const Log& want) {
    return got == want || (expect(false, message), dump(got));
}

bool initial_geometry_is_the_whole_box() {
    return check("no geometry paints the one full-box gradient", paint({{"mask-image", kRamp}}),
                 {"grad(10,10,60,60)a100-100"}) &&
           check("an unparsable geometry falls back to the initial one",
                 paint({{"mask-image", kRamp}, {"mask-size", "nonsense"}}),
                 {"grad(10,10,60,60)a100-100"});
}

bool size_and_position_place_one_tile() {
    return check("size 30x20 centred, no-repeat",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "30px 20px"},
                        {"mask-position", "center"},
                        {"mask-repeat", "no-repeat"}}),
                 {"grad(25,30,30,20)a100-100"}) &&
           check("a percentage size and a bottom-right offset",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "50% 25%"},
                        {"mask-position", "right 5px bottom 0px"},
                        {"mask-repeat", "no-repeat"}}),
                 {"grad(35,55,30,15)a100-100"}) &&
           check("cover and contain are the box for a gradient",
                 paint({{"mask-image", kRamp}, {"mask-size", "cover"}}),
                 {"grad(10,10,60,60)a100-100"});
}

bool repeat_tiles_the_box() {
    return check("a 30px tile repeats into a 2x2 grid, row-major",
                 paint({{"mask-image", kRamp}, {"mask-size", "30px 30px"}}),
                 {"grad(10,10,30,30)a100-100", "grad(40,10,30,30)a100-100",
                  "grad(10,40,30,30)a100-100", "grad(40,40,30,30)a100-100"}) &&
           check("repeat-x is one row",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "30px 30px"},
                        {"mask-repeat", "repeat-x"}}),
                 {"grad(10,10,30,30)a100-100", "grad(40,10,30,30)a100-100"}) &&
           check("an offset shifts the grid and the edge tiles are cut to the box",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "30px 60px"},
                        {"mask-position", "10px 0"},
                        {"mask-repeat", "repeat-x"}}),
                 {"clip(10,10,60,60)", "grad(-10,10,30,60)a100-100", "grad(20,10,30,60)a100-100",
                  "grad(50,10,30,60)a100-100", "unclip"});
}

bool space_and_round() {
    return check("space puts the leftover between whole tiles",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "25px 60px"},
                        {"mask-repeat", "space no-repeat"}}),
                 {"grad(10,10,25,60)a100-100", "grad(45,10,25,60)a100-100"}) &&
           check("round rescales the tile so a whole number fits",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "25px 60px"},
                        {"mask-repeat", "round no-repeat"}}),
                 {"grad(10,10,30,60)a100-100", "grad(40,10,30,60)a100-100"}) &&
           check("round goes to the nearest whole count, up as well as down",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "22px 60px"},
                        {"mask-repeat", "round no-repeat"}}),
                 {"grad(10,10,20,60)a100-100", "grad(30,10,20,60)a100-100",
                  "grad(50,10,20,60)a100-100"}) &&
           check("space with fewer than two tiles places one at the position",
                 paint({{"mask-image", kRamp},
                        {"mask-size", "40px 60px"},
                        {"mask-position", "right top"},
                        {"mask-repeat", "space no-repeat"}}),
                 {"grad(30,10,40,60)a100-100"});
}

bool layers_cycle_their_geometry() {
    // Layer 1 (listed second) is painted first. One size entry would apply to both.
    return check("per-layer sizes follow the layer order",
                 paint({{"mask-image", "linear-gradient(#000, #fff), linear-gradient(#fff, #000)"},
                        {"mask-size", "20px 20px, 40px 40px"},
                        {"mask-repeat", "no-repeat"}}),
                 {"grad(10,10,40,40)a100-100", "grad(10,10,20,20)a100-100"}) &&
           check("a short list repeats over the layers",
                 paint({{"mask-image", "linear-gradient(#000, #fff), linear-gradient(#fff, #000)"},
                        {"mask-size", "20px 20px"},
                        {"mask-repeat", "no-repeat"}}),
                 {"grad(10,10,20,20)a100-100", "grad(10,10,20,20)a100-100"});
}

bool too_many_tiles_draw_the_first_ones() {
    // 60x60 one-pixel tiles are 3600; the first 256 in row-major order are four full rows of 60
    // and the first 16 of the fifth.
    const Log log = paint({{"mask-image", kRamp}, {"mask-size", "1px 1px"}});
    bool ok = expect(log.size() == 256, "a repeat past the tile cap draws the first 256 tiles");
    ok = expect(!log.empty() && log.front() == "grad(10,10,1,1)a100-100" &&
                    log[59] == "grad(69,10,1,1)a100-100" && log[60] == "grad(10,11,1,1)a100-100" &&
                    log.back() == "grad(25,14,1,1)a100-100",
                "in row-major order") &&
         ok;
    ok = expect(g_stats.mask_layers_truncated == 1, "and the cut is counted") && ok;
    paint({{"mask-image", kRamp}, {"mask-size", "30px 30px"}});
    ok = expect(g_stats.mask_layers_truncated == 0, "a repeat that fits is not counted") && ok;
    return check("a zero-size tile draws nothing",
                 paint({{"mask-image", kRamp}, {"mask-size", "0px 10px"}}), {}) &&
           ok;
}

// A 4px border and 6px padding on the 60x60 node: padding box (14,14,52,52), content box
// (20,20,40,40).
constexpr std::initializer_list<std::pair<const char*, const char*>> kFramed = {
    {"border-top-style", "solid"},
    {"border-right-style", "solid"},
    {"border-bottom-style", "solid"},
    {"border-left-style", "solid"},
    {"border-top-width", "4px"},
    {"border-right-width", "4px"},
    {"border-bottom-width", "4px"},
    {"border-left-width", "4px"},
    {"padding-top", "6px"},
    {"padding-right", "6px"},
    {"padding-bottom", "6px"},
    {"padding-left", "6px"},
    {"mask-image", kRamp}};

Log paint_framed(std::initializer_list<std::pair<const char*, const char*>> extra) {
    std::vector<std::pair<const char*, const char*>> all(kFramed);
    all.insert(all.end(), extra.begin(), extra.end());
    Node root{1};
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    for (const auto& [property, value] : all) {
        style.set(property, value);
    }
    styles.set(2, style);
    LogSink sink;
    g_stats = paint_scene(root, styles, sink);
    return sink.log;
}

bool check_framed(const char* message,
                  std::initializer_list<std::pair<const char*, const char*>> extra,
                  const Log& want) {
    const Log got = paint_framed(extra);
    return got == want || (expect(false, message), dump(got));
}

bool origin_and_clip_boxes() {
    return check_framed("the initial boxes are the border box", {},
                        {"grad(10,10,60,60)a100-100"}) &&
           check_framed("mask-clip: padding-box cuts the border away",
                        {{"mask-clip", "padding-box"}},
                        {"clip(14,14,52,52)", "grad(10,10,60,60)a100-100", "unclip"}) &&
           check_framed("mask-origin: content-box lays the image in the content box",
                        {{"mask-origin", "content-box"}}, {"grad(20,20,40,40)a100-100"}) &&
           check_framed("a content-box tile inside a padding-box clip needs no cut",
                        {{"mask-origin", "content-box"}, {"mask-clip", "padding-box"}},
                        {"grad(20,20,40,40)a100-100"}) &&
           check_framed("a border-box tile is cut to the content-box clip",
                        {{"mask-clip", "content-box"}},
                        {"clip(20,20,40,40)", "grad(10,10,60,60)a100-100", "unclip"}) &&
           check_framed("a padding-box repeat spills past the border box and is cut to it",
                        {{"mask-origin", "padding-box"}, {"mask-size", "30px 30px"}},
                        {"clip(10,10,60,60)", "grad(14,14,30,30)a100-100",
                         "grad(44,14,30,30)a100-100", "grad(14,44,30,30)a100-100",
                         "grad(44,44,30,30)a100-100", "unclip"}) &&
           check_framed("the SVG box names map to the CSS boxes",
                        {{"mask-origin", "fill-box"}, {"mask-clip", "stroke-box"}},
                        {"grad(20,20,40,40)a100-100"});
}

bool no_clip_keeps_the_overflow() {
    return check_framed(
        "no-clip lets a tile reach past the border box",
        {{"mask-size", "30px 60px"},
         {"mask-position", "10px 0"},
         {"mask-repeat", "repeat-x"},
         {"mask-clip", "no-clip"}},
        {"grad(-10,10,30,60)a100-100", "grad(20,10,30,60)a100-100", "grad(50,10,30,60)a100-100"});
}

bool an_empty_box_paints_nothing() {
    return check_framed(
               "a content box eaten by padding is empty, so is its clip",
               {{"padding-left", "40px"}, {"padding-right", "40px"}, {"mask-clip", "content-box"}},
               {}) &&
           check_framed("and an image positioned in it",
                        {{"padding-left", "40px"},
                         {"padding-right", "40px"},
                         {"mask-origin", "content-box"}},
                        {});
}

const char* const kTwo = "linear-gradient(#000, #fff), linear-gradient(#fff, #000)";

bool composite_brackets_the_layer() {
    // Layer 0 is on top and combines with layer 1 below it, which is painted first.
    return check("add is plain painting: no bracket",
                 paint({{"mask-image", kTwo}, {"mask-composite", "add"}}),
                 {"grad(10,10,60,60)a100-100", "grad(10,10,60,60)a100-100"}) &&
           check("subtract brackets the top layer",
                 paint({{"mask-image", kTwo}, {"mask-composite", "subtract"}}),
                 {"grad(10,10,60,60)a100-100", "part(subtract)", "grad(10,10,60,60)a100-100",
                  "endpart"}) &&
           check("the bottom layer's operator is ignored",
                 paint({{"mask-image", kTwo}, {"mask-composite", "add, intersect"}}),
                 {"grad(10,10,60,60)a100-100", "grad(10,10,60,60)a100-100"}) &&
           check("intersect and exclude",
                 paint({{"mask-image", kTwo}, {"mask-composite", "exclude"}}),
                 {"grad(10,10,60,60)a100-100", "part(exclude)", "grad(10,10,60,60)a100-100",
                  "endpart"}) &&
           check("a tiled layer's tiles stay inside its bracket",
                 paint({{"mask-image", kTwo},
                        {"mask-composite", "intersect"},
                        {"mask-size", "30px 60px"},
                        {"mask-repeat", "repeat-x"}}),
                 {"grad(10,10,30,60)a100-100", "grad(40,10,30,60)a100-100", "part(intersect)",
                  "grad(10,10,30,60)a100-100", "grad(40,10,30,60)a100-100", "endpart"});
}

bool transparent_layers_still_combine() {
    const char* none_over = "none, linear-gradient(#000, #fff)";
    return check("a none layer that intersects empties what is below",
                 paint({{"mask-image", none_over}, {"mask-composite", "intersect"}}),
                 {"grad(10,10,60,60)a100-100", "part(intersect)", "endpart"}) &&
           check("so does one that subtracts",
                 paint({{"mask-image", none_over}, {"mask-composite", "subtract"}}),
                 {"grad(10,10,60,60)a100-100", "part(subtract)", "endpart"}) &&
           check("adding or excluding nothing changes nothing",
                 paint({{"mask-image", none_over}, {"mask-composite", "exclude"}}),
                 {"grad(10,10,60,60)a100-100"});
}

bool a_declined_layer_is_dropped() {
    Node root{1};
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    style.set("mask-image", kTwo);
    style.set("mask-composite", "subtract");
    styles.set(2, style);
    LogSink sink;
    sink.decline_parts = true;
    paint_scene(root, styles, sink);
    return check("a sink that declines the part loses that layer and its draws", sink.log,
                 {"grad(10,10,60,60)a100-100", "part(subtract)"});
}

bool luminance_turns_colour_into_alpha() {
    return check("black to white becomes alpha 0 to 1",
                 paint({{"mask-image", kRamp}, {"mask-mode", "luminance"}}),
                 {"grad(10,10,60,60)a0-100"}) &&
           check("alpha mode and match-source keep the stop alpha",
                 paint({{"mask-image", kRamp}, {"mask-mode", "alpha"}}),
                 {"grad(10,10,60,60)a100-100"}) &&
           check("luminance is per layer and multiplies the stop alpha",
                 paint({{"mask-image",
                         "linear-gradient(rgba(255,255,255,0.5), rgba(255,255,255,0.5)), "
                         "linear-gradient(#ffffff, #ffffff)"},
                        {"mask-mode", "luminance, alpha"}}),
                 {"grad(10,10,60,60)a100-100", "grad(10,10,60,60)a50-50"});
}

} // namespace

int main() {
    bool ok = initial_geometry_is_the_whole_box();
    ok = size_and_position_place_one_tile() && ok;
    ok = repeat_tiles_the_box() && ok;
    ok = space_and_round() && ok;
    ok = layers_cycle_their_geometry() && ok;
    ok = too_many_tiles_draw_the_first_ones() && ok;
    ok = origin_and_clip_boxes() && ok;
    ok = composite_brackets_the_layer() && ok;
    ok = transparent_layers_still_combine() && ok;
    ok = a_declined_layer_is_dropped() && ok;
    ok = no_clip_keeps_the_overflow() && ok;
    ok = an_empty_box_paints_nothing() && ok;
    ok = luminance_turns_colour_into_alpha() && ok;
    return ok ? 0 : 1;
}
