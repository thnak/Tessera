// SPDX-License-Identifier: MIT

#include "frame_time_probe.hpp"

namespace tessera::app::detail {

void FrameTimeProbe::reset(bool timestamps_usable) {
    capability_usable_ = timestamps_usable;
    pending_gpu_frame_time_ms_.reset();
}

void FrameTimeProbe::publish(std::optional<double> gpu_frame_time_ms) {
    if (!capability_usable_) {
        // An unusable capability cannot produce a usable interval; dropping it here keeps the
        // selection rule from having to re-check, and keeps a stale value from surviving a loss.
        pending_gpu_frame_time_ms_.reset();
        return;
    }
    pending_gpu_frame_time_ms_ = gpu_frame_time_ms;
}

FrameTimeObservation FrameTimeProbe::observe(double cpu_frame_time_ms) {
    // The selection rule is pure (tessera/runtime/frame_time_source.hpp); the capability decision
    // was made once by the RHI probe. A capability that is usable but has no fresh interval (the
    // first frames, a skipped read, an unusable value) falls back to the CPU time and is counted.
    const std::optional<double> gpu_frame_time_ms = pending_gpu_frame_time_ms_;
    pending_gpu_frame_time_ms_.reset();
    const runtime::FrameTimeSample sample =
        runtime::select_frame_time(cpu_frame_time_ms, gpu_frame_time_ms);
    return FrameTimeObservation{sample, gpu_frame_time_ms, capability_usable_};
}

} // namespace tessera::app::detail
