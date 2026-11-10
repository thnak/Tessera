// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/render_scale.hpp"

#include <cmath>

namespace tessera::ui::scale {
namespace {

bool valid_device_scale(float device_scale) noexcept {
    return std::isfinite(device_scale) && device_scale > 0.0f;
}

} // namespace

float quantize_render_scale(float device_scale) noexcept {
    if (!std::isfinite(device_scale)) {
        return kMinRenderScale;
    }
    if (device_scale <= kMinRenderScale) {
        return kMinRenderScale;
    }
    if (device_scale >= kMaxRenderScale) {
        return kMaxRenderScale;
    }

    // `<=` while scanning the ascending ladder makes an exact tie keep the larger step, which is
    // the documented "ties up" rule (1.125 -> 1.25, 1.375 -> 1.5, 1.75 -> 2.0).
    std::size_t best = 0;
    float best_distance = std::abs(device_scale - kRenderScaleLadder.front());
    for (std::size_t i = 1; i < kRenderScaleStepCount; ++i) {
        const float distance = std::abs(device_scale - kRenderScaleLadder[i]);
        if (distance <= best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return kRenderScaleLadder[best];
}

std::expected<float, RenderScaleError> quantize_device_scale(float device_scale) noexcept {
    if (!valid_device_scale(device_scale)) {
        return std::unexpected(RenderScaleError::InvalidDeviceScale);
    }
    return quantize_render_scale(device_scale);
}

std::expected<float, RenderScaleError> device_scale_for_dpi(std::uint32_t dpi) noexcept {
    if (dpi == 0) {
        return std::unexpected(RenderScaleError::InvalidDpi);
    }
    return static_cast<float>(dpi) / static_cast<float>(kReferenceDpi);
}

std::expected<float, RenderScaleError> render_scale_for_dpi(std::uint32_t dpi) noexcept {
    const std::expected<float, RenderScaleError> device_scale = device_scale_for_dpi(dpi);
    if (!device_scale) {
        return std::unexpected(device_scale.error());
    }
    return quantize_render_scale(*device_scale);
}

std::int32_t ladder_index(float render_scale) noexcept {
    for (std::size_t i = 0; i < kRenderScaleStepCount; ++i) {
        if (render_scale == kRenderScaleLadder[i]) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

} // namespace tessera::ui::scale
