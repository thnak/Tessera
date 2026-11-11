// SPDX-License-Identifier: MIT
//
// Deterministic pixel corpus for the software backend's CpuCanvas (src/rendering/software).
//
// CpuCanvas is the v1 software golden reference. It implements exactly three primitives: an
// axis-aligned solid quad with hardware-style pixel-center edges, an 8-bit coverage blend (the text
// reference), and an integer nearest-sample textured-quad blit with tint and clip. This file pins
// every one of them plus the boundary geometry the golden corpus relies on (zero-size, off-canvas,
// one-pixel, negative origins, half-pixel edges, alpha order) against exact RGBA8 bytes. No GPU,
// font, clock, or network is involved.
//
// Deliberate gaps of the canvas (documented in docs/guides/testing.md 4.1): it has no rounded/SDF
// rects, no rotation/scale/skew rasterizer, no gradient or shadow, and no clip stack of its own -
// clipping is a per-call rect supplied by the sink. The sink-level approximations those force are
// pinned by cpu_canvas_scene_corpus_tests.cpp.

#include "tessera/rendering/software/cpu_canvas.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::software::CpuCanvas;

using Pixel = std::array<std::uint8_t, 4>;

bool expect(bool condition, const char* case_name, const char* message) {
    if (!condition) {
        std::cerr << "cpu_canvas_corpus[" << case_name << "]: " << message << "\n";
    }
    return condition;
}

Pixel pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    return {canvas.pixels()[index], canvas.pixels()[index + 1], canvas.pixels()[index + 2],
            canvas.pixels()[index + 3]};
}

// Counts every occurrence of `expected`, so an assertion can say "exactly N pixels" instead of
// sampling one and hoping the rest did not change.
std::size_t count_pixels(const CpuCanvas& canvas, const Pixel& expected) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < canvas.pixels().size(); index += 4) {
        if (canvas.pixels()[index] == expected[0] && canvas.pixels()[index + 1] == expected[1] &&
            canvas.pixels()[index + 2] == expected[2] &&
            canvas.pixels()[index + 3] == expected[3]) {
            ++count;
        }
    }
    return count;
}

bool is_pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, const Pixel& expected) {
    return pixel(canvas, x, y) == expected;
}

constexpr Pixel kBackgroundPixel{16, 32, 48, 255};
constexpr Pixel kBlack{0, 0, 0, 255};
constexpr Pixel kAccentPixel{255, 128, 0, 255};
constexpr Pixel kWhite{255, 255, 255, 255};

CpuCanvas make_canvas(std::uint32_t width, std::uint32_t height, const Pixel& fill) {
    CpuCanvas canvas(width, height);
    canvas.clear(rgba8(fill[0], fill[1], fill[2], fill[3]));
    return canvas;
}

// Solid quads: exact coverage of an integer rect and nothing outside it.
bool check_solid_quads() {
    constexpr const char* kCase = "solid-quad";
    CpuCanvas canvas = make_canvas(6, 4, kBackgroundPixel);
    canvas.fill_rect({1, 1, 2, 2}, rgba8(255, 128, 0));
    if (!expect(count_pixels(canvas, kAccentPixel) == 4, kCase, "exactly four accent pixels")) {
        return false;
    }
    return expect(is_pixel(canvas, 1, 1, kAccentPixel) && is_pixel(canvas, 2, 2, kAccentPixel),
                  kCase, "the whole quad is filled") &&
           expect(is_pixel(canvas, 0, 1, kBackgroundPixel) &&
                      is_pixel(canvas, 3, 1, kBackgroundPixel) &&
                      is_pixel(canvas, 1, 0, kBackgroundPixel) &&
                      is_pixel(canvas, 1, 3, kBackgroundPixel),
                  kCase, "nothing outside the quad is touched");
}

// Fractional edges use the documented pixel-center rule: a pixel is filled when its (x+0.5, y+0.5)
// center lies in [left, right) x [top, bottom).
bool check_pixel_center_edges() {
    constexpr const char* kCase = "pixel-center";
    CpuCanvas canvas = make_canvas(8, 4, kBlack);
    canvas.fill_rect({3.4f, 0.6f, 2.2f, 1.4f}, rgba8(255, 255, 255));
    if (!expect(count_pixels(canvas, kWhite) == 3, kCase,
                "a 2.2 x 1.4 rect covers three centers")) {
        return false;
    }
    return expect(is_pixel(canvas, 3, 1, kWhite) && is_pixel(canvas, 4, 1, kWhite) &&
                      is_pixel(canvas, 5, 1, kWhite),
                  kCase, "row 1 columns 3-5 are filled") &&
           expect(is_pixel(canvas, 3, 0, kBlack) && is_pixel(canvas, 3, 2, kBlack) &&
                      is_pixel(canvas, 6, 1, kBlack),
                  kCase, "centers outside the half-open range are skipped");
}

// Half-pixel edges: only the pixel whose center is exactly on the left/top edge is covered.
bool check_half_pixel_edges() {
    constexpr const char* kCase = "half-pixel";
    CpuCanvas canvas = make_canvas(4, 4, kBlack);
    canvas.fill_rect({0.5f, 0.5f, 1.0f, 1.0f}, rgba8(255, 255, 255));
    return expect(count_pixels(canvas, kWhite) == 1, kCase, "one center lies in the unit rect") &&
           expect(is_pixel(canvas, 0, 0, kWhite), kCase, "the center on the edge is included") &&
           expect(is_pixel(canvas, 1, 1, kBlack), kCase, "the far center is excluded");
}

// Straight-alpha source-over with the documented float-then-round channel math. Reversing the draw
// order must produce different bytes, which proves the order is actually significant.
bool check_alpha_blend_order() {
    constexpr const char* kCase = "alpha-order";
    CpuCanvas red_over_blue = make_canvas(1, 1, {0, 0, 255, 255});
    red_over_blue.fill_rect({0, 0, 1, 1}, Color{1.0f, 0.0f, 0.0f, 0.75f});
    CpuCanvas blue_over_red = make_canvas(1, 1, {255, 0, 0, 255});
    blue_over_red.fill_rect({0, 0, 1, 1}, Color{0.0f, 0.0f, 1.0f, 0.75f});

    CpuCanvas stacked = make_canvas(1, 1, kBlack);
    stacked.fill_rect({0, 0, 1, 1}, Color{1.0f, 1.0f, 1.0f, 0.5f});
    stacked.fill_rect({0, 0, 1, 1}, Color{0.0f, 0.0f, 0.0f, 0.5f});

    return expect(is_pixel(red_over_blue, 0, 0, Pixel{191, 0, 64, 255}), kCase,
                  "alpha 0.75 red over opaque blue") &&
           expect(is_pixel(blue_over_red, 0, 0, Pixel{64, 0, 191, 255}), kCase,
                  "the reversed order differs, so order is significant") &&
           expect(is_pixel(stacked, 0, 0, Pixel{64, 64, 64, 255}), kCase,
                  "half white then half black over black stacks to 64");
}

// Zero-size, one-pixel, off-canvas, and negative-origin geometry must not crash or wrap.
bool check_boundary_geometry() {
    constexpr const char* kCase = "boundary";
    CpuCanvas canvas = make_canvas(8, 4, kBlack);
    canvas.fill_rect({2, 2, 0, 5}, rgba8(255, 0, 0));
    canvas.fill_rect({2, 2, 5, 0}, rgba8(255, 0, 0));
    const bool zero_size_noop = count_pixels(canvas, Pixel{255, 0, 0, 255}) == 0;

    // Negative origin: the half-open pixel-center clip keeps only columns/rows 0..1.
    canvas.fill_rect({-2, -2, 4, 4}, rgba8(0, 255, 0));
    const bool negative_clipped = count_pixels(canvas, Pixel{0, 255, 0, 255}) == 4 &&
                                  is_pixel(canvas, 1, 1, Pixel{0, 255, 0, 255}) &&
                                  is_pixel(canvas, 2, 2, kBlack);

    // A quad that straddles the right edge keeps exactly one canvas column.
    CpuCanvas edge = make_canvas(8, 4, kBlack);
    edge.fill_rect({7, 0, 10, 4}, rgba8(0, 128, 255));
    const bool right_edge = count_pixels(edge, Pixel{0, 128, 255, 255}) == 4 &&
                            is_pixel(edge, 7, 3, Pixel{0, 128, 255, 255});

    // Entirely outside: nothing is written (no wrap-around into the opposite corner).
    CpuCanvas outside = make_canvas(4, 4, kBlack);
    outside.fill_rect({-5, 0, 4, 4}, rgba8(255, 255, 255));
    outside.fill_rect({100, 100, 5, 5}, rgba8(255, 255, 255));
    const bool fully_outside = count_pixels(outside, kWhite) == 0;

    // One-pixel target: a full-surface clear writes exactly one opaque pixel.
    CpuCanvas single(1, 1);
    single.clear(rgba8(10, 20, 30));
    const bool single_pixel =
        single.pixels().size() == 4 && is_pixel(single, 0, 0, Pixel{10, 20, 30, 255});

    return expect(zero_size_noop, kCase, "a zero-width or zero-height quad is a no-op") &&
           expect(negative_clipped, kCase, "a negative origin clips instead of wrapping") &&
           expect(right_edge, kCase, "a quad straddling the right edge keeps one column") &&
           expect(fully_outside, kCase, "off-canvas quads write nothing") &&
           expect(single_pixel, kCase, "a 1x1 canvas clears to one opaque pixel");
}

// Nearest-sample textured quad: the documented pixel-center mapping is blocky on an integer scale.
bool check_textured_quad_nearest() {
    constexpr const char* kCase = "texture-nearest";
    const std::vector<std::uint8_t> rgba = {
        255, 0,   0,   255, // (0,0) red
        0,   255, 0,   255, // (1,0) green
        0,   0,   255, 255, // (2,0) blue
        255, 255, 0,   255, // (0,1) yellow
        255, 0,   255, 255, // (1,1) magenta
        0,   255, 255, 255, // (2,1) cyan
    };
    CpuCanvas canvas = make_canvas(6, 4, kBlack);
    canvas.blit_texture(0, 0, 6, 4, rgba, 3, 2, rgba8(255, 255, 255, 255));
    if (!expect(count_pixels(canvas, kBlack) == 0, kCase,
                "the dest quad covers the whole canvas")) {
        return false;
    }
    return expect(is_pixel(canvas, 0, 0, Pixel{255, 0, 0, 255}) &&
                      is_pixel(canvas, 1, 1, Pixel{255, 0, 0, 255}),
                  kCase, "the top-left source pixel fills its 2x2 block") &&
           expect(is_pixel(canvas, 2, 0, Pixel{0, 255, 0, 255}) &&
                      is_pixel(canvas, 4, 1, Pixel{0, 0, 255, 255}),
                  kCase, "each column maps to one source column") &&
           expect(is_pixel(canvas, 0, 2, Pixel{255, 255, 0, 255}) &&
                      is_pixel(canvas, 3, 3, Pixel{255, 0, 255, 255}) &&
                      is_pixel(canvas, 4, 3, Pixel{0, 255, 255, 255}),
                  kCase, "the bottom row maps to the last source row");
}

// Tint multiplies every sampled channel and the sampled alpha; a clip rect trims the destination.
bool check_textured_quad_tint_and_clip() {
    constexpr const char* kCase = "texture-tint";
    const std::vector<std::uint8_t> white = {255, 255, 255, 255};

    CpuCanvas opaque_tint = make_canvas(1, 1, kBlack);
    opaque_tint.blit_texture(0, 0, 1, 1, white, 1, 1, rgba8(128, 64, 32));
    const bool tint_ok = is_pixel(opaque_tint, 0, 0, Pixel{128, 64, 32, 255});

    CpuCanvas translucent = make_canvas(1, 1, kBlack);
    translucent.blit_texture(0, 0, 1, 1, white, 1, 1, Color{1.0f, 1.0f, 1.0f, 0.5f});
    const bool alpha_ok = is_pixel(translucent, 0, 0, Pixel{128, 128, 128, 255});

    CpuCanvas clipped = make_canvas(4, 4, kBlack);
    const Rect clip{3, 3, 1, 1};
    clipped.blit_texture(2, 2, 2, 2, white, 1, 1, rgba8(255, 0, 0), &clip);
    const bool clip_ok = count_pixels(clipped, Pixel{255, 0, 0, 255}) == 1 &&
                         is_pixel(clipped, 3, 3, Pixel{255, 0, 0, 255});

    // Negative destination origin samples the correct source pixel instead of wrapping.
    const std::vector<std::uint8_t> quad = {255, 0, 0,   255, 0,   255, 0,   255,
                                            0,   0, 255, 255, 255, 255, 255, 255};
    CpuCanvas negative = make_canvas(2, 2, kBlack);
    negative.blit_texture(-1, -1, 2, 2, quad, 2, 2, rgba8(255, 255, 255, 255));
    const bool negative_ok = is_pixel(negative, 0, 0, kWhite) && is_pixel(negative, 1, 0, kBlack) &&
                             is_pixel(negative, 0, 1, kBlack);

    return expect(tint_ok, kCase, "tint multiplies every sampled channel") &&
           expect(alpha_ok, kCase, "tint alpha blends source-over with integer rounding") &&
           expect(clip_ok, kCase, "the clip rect keeps exactly one destination pixel") &&
           expect(negative_ok, kCase, "a negative origin samples the bottom-right source pixel");
}

// Degenerate guards: nothing may be written for a zero-size quad, empty source, or empty canvas.
bool check_textured_quad_guards() {
    constexpr const char* kCase = "texture-guard";
    const std::vector<std::uint8_t> white = {255, 255, 255, 255};
    CpuCanvas canvas = make_canvas(2, 2, kBlack);
    canvas.blit_texture(0, 0, 0, 0, white, 1, 1, rgba8(255, 255, 255, 255));
    canvas.blit_texture(0, 0, 1, 1, {}, 1, 1, rgba8(255, 255, 255, 255));
    canvas.blit_texture(0, 0, 1, 1, white, 1, 1, Color{1.0f, 1.0f, 1.0f, 0.0f});
    const bool nothing_written = count_pixels(canvas, kWhite) == 0;

    CpuCanvas empty(0, 0);
    empty.clear(rgba8(255, 255, 255));
    empty.blit_texture(0, 0, 1, 1, white, 1, 1, rgba8(255, 255, 255, 255));
    empty.fill_rect({0, 0, 4, 4}, rgba8(255, 255, 255));
    const bool empty_canvas = empty.pixels().empty() && empty.width() == 0 && empty.height() == 0;

    return expect(nothing_written, kCase, "zero-size, empty-source, and zero-alpha blits write") &&
           expect(empty_canvas, kCase, "a 0x0 canvas stays empty instead of writing out of bounds");
}

// The coverage blend (the glyph reference primitive): straight-alpha source-over with the
// documented integer rounding, under a per-call clip. The full math matrix is cpu_canvas_tests.cpp;
// it is pinned here too so the corpus never depends on an unpinned primitive.
bool check_coverage_blend() {
    constexpr const char* kCase = "coverage-blend";
    CpuCanvas canvas = make_canvas(2, 1, kBlack);
    const std::vector<std::uint8_t> coverage = {255, 128};
    canvas.blend_coverage(0, 0, 2, 1, coverage, rgba8(255, 255, 255));
    const bool first_pass =
        is_pixel(canvas, 0, 0, kWhite) && is_pixel(canvas, 1, 0, Pixel{128, 128, 128, 255});

    const Rect clip{1, 0, 1, 1};
    canvas.blend_coverage(0, 0, 2, 1, coverage, rgba8(255, 0, 0), &clip);
    return expect(first_pass, kCase, "coverage 255 writes and coverage 128 halves the source") &&
           expect(is_pixel(canvas, 1, 0, Pixel{192, 64, 64, 255}), kCase,
                  "a clipped second pass blends only the covered pixel") &&
           expect(is_pixel(canvas, 0, 0, kWhite), kCase, "the pixel outside the clip is untouched");
}

// Negative controls. A deliberately shifted draw must differ, and the pixel predicate must reject a
// wrong expected value - otherwise every assertion above would be vacuous.
bool check_negative_controls() {
    constexpr const char* kCase = "negative-control";
    CpuCanvas left = make_canvas(4, 2, kBlack);
    left.fill_rect({1, 0, 1, 2}, rgba8(255, 0, 0));
    CpuCanvas shifted = make_canvas(4, 2, kBlack);
    shifted.fill_rect({2, 0, 1, 2}, rgba8(255, 0, 0));

    bool differ = false;
    for (std::size_t index = 0; index < left.pixels().size(); ++index) {
        differ = differ || left.pixels()[index] != shifted.pixels()[index];
    }

    return expect(differ, kCase, "a one-pixel shift changes the framebuffer") &&
           expect(is_pixel(left, 1, 0, Pixel{255, 0, 0, 255}), kCase,
                  "the painted column matches") &&
           expect(!is_pixel(left, 2, 0, Pixel{255, 0, 0, 255}), kCase,
                  "the predicate rejects the shifted column");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_solid_quads() && ok;
    ok = check_pixel_center_edges() && ok;
    ok = check_half_pixel_edges() && ok;
    ok = check_alpha_blend_order() && ok;
    ok = check_boundary_geometry() && ok;
    ok = check_textured_quad_nearest() && ok;
    ok = check_textured_quad_tint_and_clip() && ok;
    ok = check_textured_quad_guards() && ok;
    ok = check_coverage_blend() && ok;
    ok = check_negative_controls() && ok;
    if (!ok) {
        std::cerr << "cpu_canvas_corpus: failed\n";
        return 1;
    }
    std::cout << "cpu_canvas_corpus: ok\n";
    return 0;
}
