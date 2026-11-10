// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/scale/render_scale.hpp"

#include <cstdint>
#include <expected>

namespace tessera::ui::scale {

// Why a render scale was recomputed. `None` is the per-frame path: the metrics matched the cached
// device scale, so no quantization ran.
enum class RenderScaleChange : std::uint8_t {
    None = 0,
    // First accepted WindowMetrics snapshot.
    Adopted,
    // WindowMetrics::dpi_scale changed: the window moved to a display with another scale, or the
    // user changed the OS scale factor.
    DpiChange,
    // The platform reported a monitor/display change that kept the same dpi scale.
    MonitorChange,
};

// The outcome of one observation. `ladder_changed` is the only field that invalidates raster work:
// a DPI change between two values that quantize to the same step (1.20 -> 1.24) keeps both the
// render targets and the glyph atlases.
struct RenderScaleTransition {
    RenderScaleChange change = RenderScaleChange::None;
    bool ladder_changed = false;
    float device_scale = kMinRenderScale;
    float render_scale = kMinRenderScale;
    std::int32_t ladder_index = -1;
    std::uint32_t revision = 0;
};

// Per-window device/render-scale state (design/window-resize.md §2.3, design/performance.md §3).
// One instance per window, owned by the runtime beside the WindowMetrics snapshot it derives from.
//
// Contract:
// - The ladder step is recomputed only when the reported dpi scale changes or the platform reports
//   a monitor change. Feeding an unchanged WindowMetrics returns change=None: no quantization, no
//   revision bump, no atlas work. Callers may therefore observe on every tick.
// - An invalid dpi scale (non-finite or <= 0) is rejected: the call returns an error and the state,
//   revision, and counters stay exactly as they were, so one bad metrics frame cannot drop a live
//   atlas or resize a render target.
// - `revision` advances only when the quantized step changes, so consumers keyed by it never mint a
//   duplicate atlas for a fractional re-query such as 1.20 -> 1.24.
class WindowRenderScale {
public:
    WindowRenderScale() = default;

    // Frame path: compares `metrics.dpi_scale` with the cached value.
    [[nodiscard]] std::expected<RenderScaleTransition, RenderScaleError>
    observe(const hal::WindowMetrics& metrics) noexcept;

    // Infrastructure path: the window moved to another display of the same scale, which
    // WindowMetrics cannot express because it carries no display id. The step is re-quantized and
    // the transition reports MonitorChange; `ladder_changed` stays false unless the metrics also
    // changed the step.
    [[nodiscard]] std::expected<RenderScaleTransition, RenderScaleError>
    notify_display_change(const hal::WindowMetrics& metrics) noexcept;

    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    [[nodiscard]] float device_scale() const noexcept {
        return device_scale_;
    }
    [[nodiscard]] float render_scale() const noexcept {
        return render_scale_;
    }
    // Index of render_scale() in kRenderScaleLadder, or -1 before the first accepted snapshot.
    [[nodiscard]] std::int32_t ladder_index() const noexcept {
        return ladder_index_;
    }
    [[nodiscard]] std::uint32_t revision() const noexcept {
        return revision_;
    }
    // How many snapshots were quantized (diagnostics and tests); unchanged while only identical
    // frame snapshots arrive.
    [[nodiscard]] std::uint32_t recompute_count() const noexcept {
        return recompute_count_;
    }
    [[nodiscard]] std::uint32_t rejected_count() const noexcept {
        return rejected_count_;
    }

private:
    std::expected<RenderScaleTransition, RenderScaleError>
    apply(const hal::WindowMetrics& metrics, RenderScaleChange reason, bool force) noexcept;

    bool initialized_ = false;
    float device_scale_ = kMinRenderScale;
    float render_scale_ = kMinRenderScale;
    std::int32_t ladder_index_ = -1;
    std::uint32_t revision_ = 0;
    std::uint32_t recompute_count_ = 0;
    std::uint32_t rejected_count_ = 0;
};

} // namespace tessera::ui::scale
