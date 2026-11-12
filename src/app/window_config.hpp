// SPDX-License-Identifier: MIT
//
// The runtime-config keys the window driver consumes (docs/specs/runtime-config-v1.md section 6.2).
// Pure mapping only: no GPU, no window, no allocation. WindowHost calls it once at construction;
// unit tests exercise it directly, and the pipeline test drives the mapped FramePipelineConfig
// through the paint seam.

#pragma once

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

} // namespace tessera::app
