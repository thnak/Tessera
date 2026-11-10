// SPDX-License-Identifier: MIT
//
// Vulkan-free pixel check of CSS box-shadow through the deterministic CPU sink: the blurred shadow
// fades monotonically away from the box edge and vanishes beyond its reach, a sharp offset shadow
// is exact, the box itself covers the shadow beneath it, and two renders are byte-identical.

#include "cpu_paint_sink.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Node;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kWidth = 160;
constexpr std::uint32_t kHeight = 120;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_shadow_cpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    explicit StubStyles(ComputedStyle style)
        : style_(std::move(style)) {}
    ComputedStyle compute(const Node&) override {
        return style_;
    }

private:
    ComputedStyle style_;
};

std::vector<std::uint8_t> render(const char* shadow, const char* background) {
    Node node(1);
    node.set_bounds({40, 30, 80, 60});
    ComputedStyle style;
    style.set("box-shadow", shadow);
    style.set("background-color", background);
    StubStyles styles(style);
    CpuCanvas canvas(kWidth, kHeight);
    canvas.clear({1.0f, 1.0f, 1.0f, 1.0f});
    CpuPaintSink sink(canvas);
    paint_scene(node, styles, sink);
    return std::vector<std::uint8_t>(canvas.pixels().begin(), canvas.pixels().end());
}

int red_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    return pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4];
}

bool blur_fades_with_distance() {
    const auto pixels = render("0 0 16px #000000", "#ff0000");
    bool ok = expect(red_at(pixels, 80, 60) == 255 && red_at(pixels, 41, 31) == 255,
                     "the opaque box covers the shadow beneath it");
    // Walk left from the box edge (x = 39) to the far white (x = 10): never gets darker going out.
    int previous = red_at(pixels, 39, 60);
    bool monotone = true;
    for (int x = 38; x >= 10; --x) {
        const int value = red_at(pixels, static_cast<std::uint32_t>(x), 60);
        monotone = monotone && value >= previous;
        previous = value;
    }
    ok = expect(monotone, "the shadow gets lighter with distance from the box") && ok;
    ok =
        expect(red_at(pixels, 39, 60) < 200, "the shadow is clearly visible at the box edge") && ok;
    ok = expect(red_at(pixels, 39, 60) > 0, "the blurred edge is not solid black") && ok;
    ok = expect(red_at(pixels, 10, 60) == 255 && red_at(pixels, 150, 60) == 255 &&
                    red_at(pixels, 80, 5) == 255,
                "beyond the blur reach the canvas is untouched") &&
         ok;
    return ok;
}

bool sharp_offset_is_exact() {
    const auto pixels = render("10px 10px 0 #000000", "#ff0000");
    return expect(red_at(pixels, 125, 95) == 0, "inside the offset shadow, outside the box") &&
           expect(red_at(pixels, 125, 35) == 255, "right of the box above the shadow is white") &&
           expect(red_at(pixels, 45, 95) == 255, "below the box left of the shadow is white") &&
           expect(red_at(pixels, 60, 60) == 255, "the box is drawn over the shadow") &&
           expect(red_at(pixels, 131, 95) == 255, "the shadow ends at the offset edge");
}

bool deterministic() {
    return expect(render("0 0 12px rgba(0, 0, 0, 0.6)", "#336699") ==
                      render("0 0 12px rgba(0, 0, 0, 0.6)", "#336699"),
                  "two renders are byte-identical");
}

} // namespace

int main() {
    bool ok = true;
    ok = blur_fades_with_distance() && ok;
    ok = sharp_offset_is_exact() && ok;
    ok = deterministic() && ok;
    if (!ok) {
        std::cerr << "paint_shadow_cpu: FAILED\n";
        return 1;
    }
    std::cout << "paint_shadow_cpu: ok\n";
    return 0;
}
