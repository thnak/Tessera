// SPDX-License-Identifier: MIT
//
// `CpuPaintSink` rounded corners (issue #617): a rounded rect, a rounded clip and the rounded
// border box of a gradient are exact, not approximated by their bounds. The hard mode (the
// renderer's golden mode) is checked pixel for pixel against an independent circle test; the
// anti-aliased mode against the analytic smoothstep of the signed distance. Vulkan-free.

#include "cpu_wide_scene_support.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;
using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::LinearGradientStop;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::RoundedClip;

constexpr Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};
constexpr std::uint32_t kSize = 32;

CpuCanvas blue_page() {
    CpuCanvas canvas(kSize, kSize);
    canvas.clear(kBlue);
    return canvas;
}

bool is_red(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    return channel(canvas, x, y, 0) > 250 && channel(canvas, x, y, 2) < 5;
}

bool is_blue(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    return channel(canvas, x, y, 0) < 5 && channel(canvas, x, y, 2) > 250;
}

// A circle-corner box [x0, x0+w) x [y0, y0+h) with radius r: the pixel centre is inside unless it
// lies in a corner square and farther than r from that corner's circle centre. Independent of the
// signed-distance code under test.
bool inside_box(float x0, float y0, float w, float h, float r, std::uint32_t px, std::uint32_t py) {
    const float x = static_cast<float>(px) + 0.5f;
    const float y = static_cast<float>(py) + 0.5f;
    if (x < x0 || y < y0 || x > x0 + w || y > y0 + h) {
        return false;
    }
    const float cx = x < x0 + r ? x0 + r : (x > x0 + w - r ? x0 + w - r : x);
    const float cy = y < y0 + r ? y0 + r : (y > y0 + h - r ? y0 + h - r : y);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

int mismatches(const CpuCanvas& canvas, float x0, float y0, float w, float h, float r) {
    int count = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool inside = inside_box(x0, y0, w, h, r, x, y);
            if (inside ? !is_red(canvas, x, y) : !is_blue(canvas, x, y)) {
                ++count;
            }
        }
    }
    return count;
}

bool rounded_rect_is_exact() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.fill_rounded_rect({3, 3, 26, 22}, kRed, 9.0f);
    bool ok = expect(is_blue(canvas, 3, 3), "rounded rect: the corner pixel is the page");
    ok = expect(is_red(canvas, 16, 14), "rounded rect: the middle is filled") && ok;
    const int bad = mismatches(canvas, 3, 3, 26, 22, 9.0f);
    return expect(bad == 0,
                  "rounded rect: " + std::to_string(bad) + " pixels differ from the circle") &&
           ok;
}

bool rounded_clip_cuts_a_rect_and_pops() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.push_clip(RoundedClip{{4, 4, 24, 24}, 10.0f, 10.0f, 10.0f, 10.0f});
    sink.fill_rect({0, 0, 32, 32}, kRed);
    sink.pop_clip();
    const int bad = mismatches(canvas, 4, 4, 24, 24, 10.0f);
    bool ok =
        expect(bad == 0, "rounded clip: " + std::to_string(bad) + " pixels differ from the circle");
    sink.fill_rect({0, 0, 4, 4}, kRed);
    ok = expect(is_red(canvas, 1, 1), "pop_clip: a later rect is no longer cut") && ok;
    return ok;
}

bool nested_clips_multiply() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.push_clip(RoundedClip{{0, 0, 32, 32}, 14.0f, 14.0f, 14.0f, 14.0f});
    sink.push_clip(RoundedClip{{4, 4, 28, 28}, 0.0f, 0.0f, 12.0f, 0.0f});
    sink.fill_rect({0, 0, 32, 32}, kRed);
    sink.pop_clip();
    sink.pop_clip();
    bool ok = expect(is_blue(canvas, 0, 0), "nested: the outer clip cuts the top-left corner");
    ok = expect(is_blue(canvas, 2, 16), "nested: the inner rect's left edge cuts at x=4") && ok;
    ok = expect(is_blue(canvas, 31, 31), "nested: the inner bottom-right radius cuts") && ok;
    ok = expect(is_red(canvas, 16, 16), "nested: the middle is filled") && ok;
    return ok;
}

bool gradient_follows_its_rounded_clip() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    GradientDraw draw;
    draw.rect = {2, 2, 28, 28};
    draw.clip = RoundedClip{draw.rect, 11.0f, 11.0f, 11.0f, 11.0f};
    draw.angle_degrees = 90.0f;
    draw.stops[0] = LinearGradientStop{0.0f, kRed};
    draw.stops[1] = LinearGradientStop{1.0f, kRed};
    draw.stop_count = 2;
    sink.fill_linear_gradient(draw);
    const int bad = mismatches(canvas, 2, 2, 28, 28, 11.0f);
    return expect(bad == 0, "gradient: " + std::to_string(bad) + " pixels differ from the circle");
}

// The anti-aliased mode: a pixel is red by 1 - smoothstep(-0.5, 0.5, signed distance). The check
// recomputes the distance from the circle's centre, an independent form of the box SDF.
bool anti_aliased_edge_is_the_smoothstep() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.set_aa_radius(0.5f);
    sink.fill_rounded_rect({2, 2, 28, 28}, kRed, 12.0f);
    int partial = 0;
    bool ok = true;
    for (std::uint32_t y = 2; y < 14; ++y) {
        for (std::uint32_t x = 2; x < 14; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - 14.0f;
            const float dy = static_cast<float>(y) + 0.5f - 14.0f;
            const float distance = std::sqrt(dx * dx + dy * dy) - 12.0f;
            const float t = std::clamp((distance + 0.5f) / 1.0f, 0.0f, 1.0f);
            const float coverage = 1.0f - t * t * (3.0f - 2.0f * t);
            const int expected = static_cast<int>(std::lround(coverage * 255.0f));
            const int actual = channel(canvas, x, y, 0);
            if (std::abs(actual - expected) > 2) {
                ok = expect(false, "aa: (" + std::to_string(x) + "," + std::to_string(y) +
                                       ") red " + std::to_string(actual) + " expected " +
                                       std::to_string(expected)) &&
                     ok;
            }
            partial += actual > 10 && actual < 245 ? 1 : 0;
        }
    }
    return expect(partial > 4, "aa: the corner arc has blended pixels") && ok;
}

} // namespace

int main() {
    bool ok = rounded_rect_is_exact();
    ok = rounded_clip_cuts_a_rect_and_pops() && ok;
    ok = nested_clips_multiply() && ok;
    ok = gradient_follows_its_rounded_clip() && ok;
    ok = anti_aliased_edge_is_the_smoothstep() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "cpu_rounded_sink: ok\n";
    return 0;
}
