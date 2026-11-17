// SPDX-License-Identifier: MIT
//
// Glyph quad placement in CSS space (docs/design/display-aware-rendering.md section 3.1). The atlas
// rasterizes a glyph at its device pixel size, so its bearings and bitmap are device pixels while
// the pen, the baseline and every layout metric are CSS pixels. This is the one place the two
// spaces meet; it is internal (not public API) so a unit test can pin the conversion.

#pragma once

#include <cmath>
#include <cstdint>

namespace tessera::ui::text::detail {

// The device pixel size a CSS font size rasterizes at (section 3.1): rounded to the nearest device
// pixel, at least one, and inside the rasterizer's uint16 range. A non-finite or non-positive scale
// falls back to 1 device pixel per CSS pixel instead of producing an invalid size.
[[nodiscard]] inline std::uint16_t device_pixel_size(std::uint16_t css_pixel_size,
                                                     float render_scale) noexcept {
    const float scale = render_scale > 0.0f && std::isfinite(render_scale) ? render_scale : 1.0f;
    const float scaled = static_cast<float>(css_pixel_size) * scale;
    // A CSS size below one pixel would otherwise rasterize at 0, which the rasterizer rejects.
    if (scaled < 1.0f) {
        return 1;
    }
    if (scaled >= 65535.0f) {
        return 65535;
    }
    return static_cast<std::uint16_t>(std::lround(scaled));
}

struct GlyphQuad {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Places one glyph bitmap back into the caller's CSS space. `pen_css`/`baseline_css` and the two
// offsets are CSS pixels; a bearing and a bitmap extent are device pixels. Snapping happens in
// device space - a hinted stem must land on a device pixel, not on a CSS one, at any scale - and
// the result divides by the same scale. At render_scale 1.0 every expression is today's whole-pixel
// snap; with subpixel positioning the bitmap already carries the fraction, so the origin sits on
// the floor exactly as before.
[[nodiscard]] inline GlyphQuad
place_glyph_quad(float pen_css, float baseline_css, float x_offset_css, float y_offset_css,
                 std::int32_t bearing_x_device, std::int32_t bearing_y_device,
                 std::uint32_t width_device, std::uint32_t height_device, float render_scale,
                 bool subpixel_position) noexcept {
    const float scale = render_scale > 0.0f && std::isfinite(render_scale) ? render_scale : 1.0f;
    GlyphQuad quad;
    if (subpixel_position) {
        quad.left =
            (std::floor((pen_css + x_offset_css) * scale) + static_cast<float>(bearing_x_device)) /
            scale;
    } else {
        quad.left =
            std::round((pen_css + x_offset_css) * scale + static_cast<float>(bearing_x_device)) /
            scale;
    }
    quad.top =
        std::round((baseline_css - y_offset_css) * scale - static_cast<float>(bearing_y_device)) /
        scale;
    quad.width = static_cast<float>(width_device) / scale;
    quad.height = static_cast<float>(height_device) / scale;
    return quad;
}

} // namespace tessera::ui::text::detail
