// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/window_render_scale.hpp"

#include <cmath>

namespace tessera::ui::scale {

std::expected<RenderScaleTransition, RenderScaleError>
WindowRenderScale::observe(const hal::WindowMetrics& metrics) noexcept {
    return apply(metrics, RenderScaleChange::DpiChange, false);
}

std::expected<RenderScaleTransition, RenderScaleError>
WindowRenderScale::notify_display_change(const hal::WindowMetrics& metrics) noexcept {
    return apply(metrics, RenderScaleChange::MonitorChange, true);
}

std::expected<RenderScaleTransition, RenderScaleError>
WindowRenderScale::apply(const hal::WindowMetrics& metrics, RenderScaleChange reason,
                         bool force) noexcept {
    const float device_scale = metrics.dpi_scale;
    if (!std::isfinite(device_scale) || device_scale <= 0.0f) {
        ++rejected_count_;
        return std::unexpected(RenderScaleError::InvalidDeviceScale);
    }

    RenderScaleTransition transition;
    if (initialized_ && !force && device_scale == device_scale_) {
        // Same snapshot as the previous frame: the cached step is already in effect, so this is a
        // pure flag return with no quantization and no revision bump.
        transition.device_scale = device_scale_;
        transition.render_scale = render_scale_;
        transition.ladder_index = ladder_index_;
        transition.revision = revision_;
        return transition;
    }

    const float render_scale = quantize_render_scale(device_scale);
    ++recompute_count_;

    const bool ladder_changed = !initialized_ || render_scale != render_scale_;
    if (ladder_changed) {
        render_scale_ = render_scale;
        ladder_index_ = render_scale_index(render_scale_);
        ++revision_;
    }
    device_scale_ = device_scale;

    const bool first = !initialized_;
    initialized_ = true;

    transition.change = first ? RenderScaleChange::Adopted : reason;
    transition.ladder_changed = ladder_changed;
    transition.device_scale = device_scale_;
    transition.render_scale = render_scale_;
    transition.ladder_index = ladder_index_;
    transition.revision = revision_;
    return transition;
}

} // namespace tessera::ui::scale
