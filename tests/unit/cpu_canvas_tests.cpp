// SPDX-License-Identifier: MIT
//
// Pure CPU coverage-blend math for software text goldens: straight-alpha source-over with the
// documented integer rounding (coverage 0/255/intermediate, alpha modulation, overlap, clipping,
// off-canvas blits). No font/shaping here; text-specific tests live in the paint/text suites.

#include "tessera/rendering/software/cpu_canvas.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::software::CpuCanvas;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cpu_canvas: " << message << "\n";
    }
    return condition;
}

std::array<std::uint8_t, 4> pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    return {canvas.pixels()[index], canvas.pixels()[index + 1], canvas.pixels()[index + 2],
            canvas.pixels()[index + 3]};
}

CpuCanvas black_canvas(std::uint32_t width, std::uint32_t height) {
    CpuCanvas canvas(width, height);
    canvas.clear(rgba8(0, 0, 0, 255));
    return canvas;
}

bool test_coverage_extremes() {
    CpuCanvas canvas = black_canvas(2, 1);
    const std::uint8_t zero = 0;
    const std::uint8_t full = 255;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&zero, 1),
                          rgba8(255, 255, 255, 255));
    canvas.blend_coverage(1, 0, 1, 1, std::span<const std::uint8_t>(&full, 1),
                          rgba8(255, 255, 255, 255));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "coverage 0 leaves the destination untouched") &&
           expect(pixel(canvas, 1, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
                  "opaque source at coverage 255 writes the source color");
}

bool test_intermediate_coverage_rounding() {
    CpuCanvas canvas = black_canvas(1, 1);
    const std::uint8_t half = 128;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&half, 1),
                          rgba8(255, 255, 255, 255));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{128, 128, 128, 255},
                  "coverage 128 over black rounds to 128");
}

bool test_straight_alpha_color() {
    CpuCanvas canvas = black_canvas(1, 1);
    const std::uint8_t full = 255;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&full, 1),
                          rgba8(255, 255, 255, 128));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{128, 128, 128, 255},
                  "color alpha 128 at full coverage is a half blend");
}

bool test_overlap_and_second_pass() {
    CpuCanvas canvas = black_canvas(1, 1);
    const std::uint8_t half = 128;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&half, 1),
                          rgba8(255, 255, 255, 255));
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&half, 1), rgba8(0, 0, 0, 255));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{64, 64, 64, 255},
                  "overlapping coverage blends source-over deterministically");
}

bool test_straight_vs_premultiplied_input() {
    // The API takes straight-alpha color; a half-transparent red over opaque blue gives half red
    // and half blue (not a premultiplied additive result).
    CpuCanvas canvas(1, 1);
    canvas.clear(rgba8(0, 0, 255, 255));
    const std::uint8_t full = 255;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&full, 1),
                          rgba8(255, 0, 0, 128));
    const auto result = pixel(canvas, 0, 0);
    return expect(result[0] == 128 && result[2] == 127,
                  "straight-alpha blend uses color.a as coverage");
}

bool test_clip_rect() {
    CpuCanvas canvas = black_canvas(4, 4);
    const std::vector<std::uint8_t> coverage(4, 255);
    const Rect clip{2, 2, 1, 1};
    canvas.blend_coverage(1, 1, 2, 2, coverage, rgba8(255, 255, 255, 255), &clip);
    return expect(pixel(canvas, 2, 2) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
                  "clip keeps the covered pixel") &&
           expect(pixel(canvas, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "clip rejects the pixel outside it") &&
           expect(pixel(canvas, 3, 2) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "clip rejects columns to the right");
}

bool test_off_canvas_blit() {
    CpuCanvas canvas = black_canvas(2, 2);
    // A 2x2 bitmap placed at (-1, -1): only its bottom-right coverage lands on canvas pixel (0,0).
    const std::vector<std::uint8_t> coverage = {0, 0, 0, 255};
    canvas.blend_coverage(-1, -1, 2, 2, coverage, rgba8(255, 255, 255, 255));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
                  "negative origin clips instead of wrapping") &&
           expect(pixel(canvas, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "negative origin does not write the opposite corner");
}

bool test_guard_conditions() {
    CpuCanvas canvas = black_canvas(1, 1);
    const std::uint8_t full = 255;
    canvas.blend_coverage(0, 0, 1, 1, std::span<const std::uint8_t>(&full, 1), rgba8(255, 0, 0, 0));
    const bool transparent_noop = pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255};
    const std::vector<std::uint8_t> too_small(0);
    canvas.blend_coverage(0, 0, 2, 2, too_small, rgba8(255, 255, 255, 255));
    canvas.blend_coverage(0, 0, 0, 0, too_small, rgba8(255, 255, 255, 255));
    return expect(transparent_noop, "fully transparent color is a no-op") &&
           expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "short or empty coverage does not write");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_coverage_extremes() && ok;
    ok = test_intermediate_coverage_rounding() && ok;
    ok = test_straight_alpha_color() && ok;
    ok = test_overlap_and_second_pass() && ok;
    ok = test_straight_vs_premultiplied_input() && ok;
    ok = test_clip_rect() && ok;
    ok = test_off_canvas_blit() && ok;
    ok = test_guard_conditions() && ok;
    if (!ok) {
        std::cerr << "cpu_canvas: failed\n";
        return 1;
    }
    std::cout << "cpu_canvas: ok\n";
    return 0;
}
