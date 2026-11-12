// SPDX-License-Identifier: MIT
//
// The adaptive render-scale extent math (docs/design/performance.md section 6.1,
// docs/specs/runtime-config-v1.md section 6.2). Pure functions: no GPU, no window, no clock.
//
// Pinned rules:
//   target = max(1, floor(presented_extent * adaptive_scale))
// where `presented_extent` is the window's physical pixel grid - its logical extent times the
// device scale, rounded up to whole pixels - so the adaptive multiplier is a resample of exactly
// what the window already presents. At adaptive scale 1.0 the target is exactly the presented
// extent: the consumer takes its unchanged direct path.
//
// The device (ladder) scale is asserted separately: glyph rasterization is keyed by the quantized
// device scale and never by the adaptive multiplier, so no table row can change it.

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"

#include <cstdint>
#include <iostream>

namespace {

using tessera::rendering::rhi::Extent2D;
using tessera::ui::scale::adaptive_extent_pixels;
using tessera::ui::scale::adaptive_target_extent;
using tessera::ui::scale::glyph_raster_scale;
using tessera::ui::scale::kMaxRenderTargetExtent;
using tessera::ui::scale::presented_extent_pixels;
using tessera::ui::scale::quantize_render_scale;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "render_target_extent: " << message << "\n";
    }
    return condition;
}

bool expect_extent(Extent2D actual, std::uint32_t width, std::uint32_t height,
                   const char* message) {
    if (actual.width == width && actual.height == height) {
        return true;
    }
    std::cerr << "render_target_extent: " << message << " (got " << actual.width << "x"
              << actual.height << ", expected " << width << "x" << height << ")\n";
    return false;
}

// One table row: the logical window size, the device scale it reports, the adaptive multiplier, and
// the extent the scene must render into.
struct Row {
    float logical_width;
    float logical_height;
    float device_scale;
    float adaptive_scale;
    std::uint32_t target_width;
    std::uint32_t target_height;
};

bool extent_table() {
    // Presented extent = ceil(logical * device scale); the target is floor(presented * adaptive).
    // The fractional rows pin the floor (never a round-up that would claim an unrendered pixel) and
    // the rows at adaptive 1.0 pin exact identity, including the fractional-DPI case where the
    // presented grid is a rounded-up whole number of pixels.
    const Row rows[] = {
        {320.0f, 200.0f, 1.0f, 1.0f, 320, 200},   // identity at 96 dpi
        {320.0f, 200.0f, 2.0f, 1.0f, 640, 400},   // identity at a 2.0 device scale
        {101.0f, 77.0f, 1.25f, 1.0f, 127, 97},    // identity: 126.25/96.25 round up to the grid
        {320.0f, 200.0f, 1.0f, 0.75f, 240, 150},  // a clean downgrade
        {320.0f, 200.0f, 1.0f, 0.9f, 288, 180},   // 288.0 / 180.0 are exact
        {101.0f, 77.0f, 1.25f, 0.75f, 95, 72},    // 127*0.75 = 95.25 -> 95; 97*0.75 = 72.75 -> 72
        {33.3f, 21.7f, 1.0f, 0.9f, 30, 19},       // 34*0.9 = 30.6 -> 30; 22*0.9 = 19.8 -> 19
        {3.0f, 2.0f, 1.0f, 0.25f, 1, 1},          // the 1-pixel floor on both axes
        {1.0f, 1.0f, 1.0f, 0.25f, 1, 1},          // nothing may collapse below one pixel
        {320.0f, 200.0f, 1.0f, 0.6f, 192, 120},   // the default minRenderScale
        {100.0f, 50.0f, 1.0f, 2.0f, 200, 100},    // a renderScale above 1.0 renders up
        {320.0f, 200.0f, 1.0f, 0.875f, 280, 175}, // the controller's first ladder step
    };

    bool ok = true;
    for (const Row& row : rows) {
        const std::uint32_t presented_width =
            presented_extent_pixels(row.logical_width, row.device_scale);
        const std::uint32_t presented_height =
            presented_extent_pixels(row.logical_height, row.device_scale);
        const Extent2D target =
            adaptive_target_extent(Extent2D{presented_width, presented_height}, row.adaptive_scale)
                .extent;
        ok = expect_extent(target, row.target_width, row.target_height,
                           "table row has the documented extent") &&
             ok;
    }
    return ok;
}

bool scale_one_is_identity() {
    // Every size the window can present, at every device ladder step: adaptive 1.0 yields exactly
    // that extent, so the frame takes the direct path and no pixel changes.
    bool ok = true;
    for (const float device : tessera::ui::scale::kRenderScaleLadder) {
        for (std::uint32_t logical = 1; logical <= 300; ++logical) {
            const std::uint32_t presented =
                presented_extent_pixels(static_cast<float>(logical), device);
            const auto target = adaptive_target_extent(Extent2D{presented, presented}, 1.0f);
            if (target.extent.width != presented || target.extent.height != presented ||
                target.scaled || target.content_scale_x != 1.0f || target.content_scale_y != 1.0f) {
                std::cerr << "render_target_extent: scale 1.0 is not identity at present "
                          << presented << " device " << device << "\n";
                ok = false;
            }
        }
    }
    return ok;
}

bool one_pixel_floor_and_bad_inputs() {
    bool ok = true;
    ok = expect(adaptive_extent_pixels(1, 0.25f) == 1, "a 1-pixel target cannot shrink") && ok;
    ok = expect(adaptive_extent_pixels(0, 1.0f) == 1, "a zero presented extent clamps to 1") && ok;
    ok = expect(adaptive_extent_pixels(320, 0.0f) == 320, "a zero multiplier is not a downgrade") &&
         ok;
    ok = expect(adaptive_extent_pixels(320, -1.0f) == 320,
                "a negative multiplier is not a downgrade") &&
         ok;
    ok = expect(adaptive_extent_pixels(320, 2.0f) == 640,
                "a multiplier above 1.0 renders a larger target") &&
         ok;
    ok = expect(adaptive_extent_pixels(static_cast<std::uint32_t>(kMaxRenderTargetExtent), 4.0f) ==
                    kMaxRenderTargetExtent,
                "the extent ceiling holds") &&
         ok;
    const auto fallback = adaptive_target_extent(Extent2D{320, 200}, 0.0f);
    ok = expect(!fallback.scaled && fallback.extent.width == 320 && fallback.extent.height == 200,
                "an invalid multiplier keeps the full-quality target") &&
         ok;
    return ok;
}

bool content_scale_maps_presented_to_target() {
    const auto scaled = adaptive_target_extent(Extent2D{101, 77}, 0.75f);
    bool ok =
        expect(scaled.extent.width == 75 && scaled.extent.height == 57, "101x77 at 0.75 is 75x57");
    // The mapping must land the last presented pixel inside the target: scaling by the extent ratio
    // (not the nominal multiplier) is what makes the resample cover the whole image.
    ok = expect(scaled.content_scale_x == 75.0f / 101.0f && scaled.content_scale_y == 57.0f / 77.0f,
                "content scale is the exact extent ratio") &&
         ok;
    const float last_x = 100.0f * scaled.content_scale_x;
    const float last_y = 76.0f * scaled.content_scale_y;
    ok = expect(last_x >= 0.0f && last_x < 75.0f,
                "the last presented column maps into the target") &&
         ok;
    ok = expect(last_y >= 0.0f && last_y < 57.0f, "the last presented row maps into the target") &&
         ok;
    return ok;
}

bool device_ladder_keys_glyphs_not_the_adaptive_scale() {
    // Text/glyph rasterization is keyed by the quantized device scale. The adaptive multiplier is
    // not an input, so every downgrade step leaves it exactly where it was.
    bool ok = true;
    const float reported[] = {1.0f, 1.0833f, 1.25f, 1.4f, 1.5f, 1.75f, 2.0f};
    for (const float device : reported) {
        const float glyph_scale = glyph_raster_scale(device);
        if (glyph_scale != quantize_render_scale(device)) {
            std::cerr << "render_target_extent: glyph scale is not the device ladder\n";
            ok = false;
        }
        for (const float adaptive : {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.25f}) {
            // Rendering at `adaptive` must not move the glyph scale: the atlas stays keyed by the
            // ladder step, and the target extent is the only thing that changed.
            const auto target =
                adaptive_target_extent(Extent2D{presented_extent_pixels(320.0f, device),
                                                presented_extent_pixels(200.0f, device)},
                                       adaptive);
            if (glyph_raster_scale(device) != glyph_scale || target.extent.width == 0) {
                std::cerr << "render_target_extent: the adaptive scale disturbed rasterization\n";
                ok = false;
            }
        }
    }
    // A fractional DPI quantizes onto the ladder (1.0833 -> 1.0), which is the value glyphs use.
    ok =
        expect(glyph_raster_scale(1.0833f) == 1.0f, "a fractional dpi quantizes onto the ladder") &&
        ok;
    ok = expect(glyph_raster_scale(1.4f) == 1.5f, "ties and midpoints round up the ladder") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = extent_table();
    ok = scale_one_is_identity() && ok;
    ok = one_pixel_floor_and_bad_inputs() && ok;
    ok = content_scale_maps_presented_to_target() && ok;
    ok = device_ladder_keys_glyphs_not_the_adaptive_scale() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "render_target_extent: ok\n";
    return 0;
}
