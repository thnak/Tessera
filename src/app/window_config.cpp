// SPDX-License-Identifier: MIT

#include "window_config.hpp"

namespace tessera::app {

ui::frame::FramePipelineConfig window_pipeline_config(const RuntimeConfig& runtime) {
    ui::frame::FramePipelineConfig config;
    config.interpolation = runtime.animation_interpolation;
    return config;
}

WindowPausePolicy window_pause_policy(const RuntimeConfig& runtime) {
    WindowPausePolicy policy;
    // The one key describes an invisible window, not one particular lifecycle event: both the
    // occluded and minimized sources follow it (section 5.8).
    policy.pause_when_occluded = runtime.pause_when_occluded;
    policy.pause_when_minimized = runtime.pause_when_occluded;
    return policy;
}

} // namespace tessera::app
