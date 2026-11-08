// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/pixel_snap.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::layout {
namespace {

float effective_scale(float dpi_scale) {
    if (!std::isfinite(dpi_scale) || dpi_scale < 1.0f) {
        return 1.0f;
    }
    return dpi_scale;
}

} // namespace

float round_half_up(float value) {
    if (!std::isfinite(value)) {
        return value;
    }
    return value >= 0.0f ? std::floor(value + 0.5f) : std::ceil(value - 0.5f);
}

float snap_scalar(float value, float dpi_scale) {
    const float scale = effective_scale(dpi_scale);
    return round_half_up(value * scale) / scale;
}

tessera::rendering::rhi::Rect snap_rect(const tessera::rendering::rhi::Rect& bounds,
                                        float dpi_scale) {
    const float scale = effective_scale(dpi_scale);
    const float left = snap_scalar(bounds.x, scale);
    const float top = snap_scalar(bounds.y, scale);
    const float right = snap_scalar(bounds.x + bounds.width, scale);
    const float bottom = snap_scalar(bounds.y + bounds.height, scale);
    return tessera::rendering::rhi::Rect{left, top, right - left, bottom - top};
}

} // namespace tessera::ui::layout
