// SPDX-License-Identifier: MIT
//
// The runtime-config keys the window driver consumes (docs/specs/runtime-config-v1.md section 6.2).
// Pure mapping only: no GPU, no window, no allocation. WindowHost calls it once at construction;
// unit tests exercise it directly, and the pipeline test drives the mapped FramePipelineConfig
// through the paint seam.

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/ui/frame_pipeline.hpp"

namespace tessera::app {

// Which lifecycle states pause rendering. The runtime config has one key for an invisible window,
// `pauseWhenOccluded`, so it owns both lifecycle sources (occluded and minimized). Focus/blur is
// never part of this policy: a visible background window keeps painting.
struct WindowPausePolicy {
    bool pause_when_occluded = true;
    bool pause_when_minimized = true;

    // True when a lifecycle event or metrics snapshot must pause the driver.
    [[nodiscard]] bool pauses(bool occluded, bool minimized) const noexcept {
        return (occluded && pause_when_occluded) || (minimized && pause_when_minimized);
    }

    [[nodiscard]] bool operator==(const WindowPausePolicy&) const = default;
};

// The frame pipeline's config for one window. Only `animationInterpolation` is wired in this
// slice; every other field keeps FramePipelineConfig's default, so an unset RuntimeConfig behaves
// exactly like the hard-coded behaviour before the key existed.
[[nodiscard]] ui::frame::FramePipelineConfig window_pipeline_config(const RuntimeConfig& runtime);

// The occluded/minimized pause policy for one window. Only `pauseWhenOccluded` is wired in this
// slice; the remaining lifecycle keys stay unwired.
[[nodiscard]] WindowPausePolicy window_pause_policy(const RuntimeConfig& runtime);

// The adaptive-quality policy for one window (docs/design/performance.md section 6): the
// `adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale` and `frameBudgetMs` keys, plus
// the effective present rate that derives the budget when `frameBudgetMs` is 0. That rate is
// `maxFps` when it is capped, else `preferredRefreshRate` when requested, else the 60 Hz baseline
// the framework's default fixed timestep uses (the pacer still follows the real display; feeding it
// a later measured rate is a follow-up, not part of this slice).
//
// `refresh_hz` is the display refresh the platform reported (0 = unknown). It is the divisor base
// of the step-3 present cap and the ceiling the cap may never exceed; with 0 there is no divisor,
// so the cap stays at `maxFps` and the pacer keeps its documented deadline pacing.
[[nodiscard]] AdaptiveQualityPolicy window_adaptive_quality_policy(const RuntimeConfig& runtime,
                                                                   double refresh_hz = 0.0);

} // namespace tessera::app
