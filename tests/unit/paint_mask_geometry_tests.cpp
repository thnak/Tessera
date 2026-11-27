// SPDX-License-Identifier: MIT
//
// Mask geometry in the paint pass (issue #617): `mask-size`, `mask-position`, `mask-repeat` and
// `mask-mode` decide where each mask layer's gradient is drawn inside the node's border box. The
// stream is checked against hand-computed tiles: placement, repeat/space/round, the clip that cuts
// an overflowing tile to the box, per-layer cycling, the tile cap and luminance stops.

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
    std::vector<std::string> log;
};

using Log = std::vector<std::string>;

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
    paint_scene(root, styles, sink);
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

bool too_many_tiles_paint_nothing() {
    return check("a repeat past the tile cap draws no tile",
                 paint({{"mask-image", kRamp}, {"mask-size", "1px 1px"}}), {}) &&
           check("a zero-size tile draws nothing",
                 paint({{"mask-image", kRamp}, {"mask-size", "0px 10px"}}), {});
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
    ok = too_many_tiles_paint_nothing() && ok;
    ok = luminance_turns_colour_into_alpha() && ok;
    return ok ? 0 : 1;
}
