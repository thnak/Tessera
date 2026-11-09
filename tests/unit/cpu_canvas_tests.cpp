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

bool test_texture_nearest_sampling() {
    // 2x2 source (red, green / blue, white) blitted to 4x4: each source pixel is a 2x2 block.
    const std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    CpuCanvas canvas = black_canvas(4, 4);
    canvas.blit_texture(0, 0, 4, 4, rgba, 2, 2, rgba8(255, 255, 255, 255));
    return expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255},
                  "nearest sampling maps the top-left source pixel") &&
           expect(pixel(canvas, 1, 1) == std::array<std::uint8_t, 4>{255, 0, 0, 255},
                  "the top-left block is 2x2") &&
           expect(pixel(canvas, 3, 0) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
                  "the top-right block samples the right source column") &&
           expect(pixel(canvas, 0, 3) == std::array<std::uint8_t, 4>{0, 0, 255, 255},
                  "the bottom-left block samples the bottom source row") &&
           expect(pixel(canvas, 3, 3) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
                  "the bottom-right block samples the last source pixel");
}

bool test_texture_tint_and_alpha() {
    const std::vector<std::uint8_t> white = {255, 255, 255, 255};
    CpuCanvas tinted = black_canvas(1, 1);
    tinted.blit_texture(0, 0, 1, 1, white, 1, 1, rgba8(128, 64, 0, 255));
    const bool tint_ok = pixel(tinted, 0, 0) == std::array<std::uint8_t, 4>{128, 64, 0, 255};

    CpuCanvas translucent = black_canvas(1, 1);
    translucent.blit_texture(0, 0, 1, 1, white, 1, 1, rgba8(255, 255, 255, 128));
    const bool alpha_ok =
        pixel(translucent, 0, 0) == std::array<std::uint8_t, 4>{128, 128, 128, 255};

    const std::vector<std::uint8_t> half = {255, 0, 0, 128};
    CpuCanvas alpha_scaled = black_canvas(1, 1);
    alpha_scaled.blit_texture(0, 0, 1, 1, half, 1, 1, rgba8(255, 255, 255, 128));
    const bool half_ok = pixel(alpha_scaled, 0, 0) == std::array<std::uint8_t, 4>{64, 0, 0, 255};
    return expect(tint_ok, "tint multiplies every sampled channel") &&
           expect(alpha_ok, "tint alpha blends source-over with integer rounding") &&
           expect(half_ok, "source alpha and tint alpha compose");
}

bool test_texture_clip_and_off_canvas() {
    const std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    CpuCanvas canvas = black_canvas(4, 4);
    const Rect clip{2, 2, 1, 1};
    canvas.blit_texture(1, 1, 2, 2, rgba, 2, 2, rgba8(255, 255, 255, 255), &clip);
    const bool clip_ok = pixel(canvas, 2, 2) == std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
                         pixel(canvas, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255};

    CpuCanvas off = black_canvas(2, 2);
    off.blit_texture(-1, -1, 2, 2, rgba, 2, 2, rgba8(255, 255, 255, 255));
    return expect(clip_ok, "clip keeps only the intersecting destination pixels") &&
           expect(pixel(off, 0, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
                  "negative origin samples the correct source pixel") &&
           expect(pixel(off, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "negative origin does not write the opposite corner");
}

bool test_texture_guards() {
    CpuCanvas canvas = black_canvas(2, 2);
    const std::vector<std::uint8_t> white = {255, 255, 255, 255};
    canvas.blit_texture(0, 0, 1, 1, white, 1, 1, rgba8(255, 255, 255, 0));
    const bool transparent_noop = pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255};
    const std::vector<std::uint8_t> short_rgba = {255, 255, 255};
    canvas.blit_texture(0, 0, 2, 2, short_rgba, 1, 1, rgba8(255, 255, 255, 255));
    const bool short_ok = pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255};
    canvas.blit_texture(0, 0, 0, 0, white, 1, 1, rgba8(255, 255, 255, 255));
    return expect(transparent_noop, "zero tint alpha is a no-op") &&
           expect(short_ok, "a short source buffer does not write") &&
           expect(pixel(canvas, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
                  "degenerate destination extents do not write");
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
    ok = test_texture_nearest_sampling() && ok;
    ok = test_texture_tint_and_alpha() && ok;
    ok = test_texture_clip_and_off_canvas() && ok;
    ok = test_texture_guards() && ok;
    if (!ok) {
        std::cerr << "cpu_canvas: failed\n";
        return 1;
    }
    std::cout << "cpu_canvas: ok\n";
    return 0;
}
