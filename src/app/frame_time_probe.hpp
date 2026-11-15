// SPDX-License-Identifier: MIT
//
// The GPU frame-time signal of WindowHost::Impl, as a type (god-type audit slice 9, section 3.2).
//
// The window host used to carry three members and two methods for this: the device probe's answer,
// the interval of the last completed submission, and the selection rule that turns those plus the
// CPU time into one FrameTimeSample. None of it depends on the window, the swapchain or the frame
// loop, which is why the audit lists it as the first extraction: pure state, no lifecycle coupling.
//
// The probe never decides *whether* the device supports timestamps. That is the RHI probe's answer
// (`rhi::Device::timestamp_capability().usable()`), resolved once per presentation generation and
// handed in by `reset()`. This type only remembers it and selects between two already-produced
// numbers, so the rule stays testable with a fake capability and no device.

#pragma once

#include "tessera/runtime/frame_time_source.hpp"

#include <cstdint>
#include <optional>

namespace tessera::app::detail {

namespace runtime = tessera::runtime;

// One observation, with everything the host needs to record it: the selected sample, the GPU
// interval that was consumed (the adaptive controller wants the raw number, not just the winner),
// and whether the capability was usable at all, which is what distinguishes "the GPU was not used
// because it is unusable" from "it was usable and this frame had no fresh interval".
struct FrameTimeObservation {
    runtime::FrameTimeSample sample;
    std::optional<double> gpu_frame_time_ms;
    bool capability_usable = false;

    // True when the capability was usable but this observation fell back to the CPU time. A reader
    // can tell a real GPU number from a counted fallback.
    [[nodiscard]] bool counted_fallback() const {
        return capability_usable && sample.source == runtime::FrameTimeSource::Cpu;
    }
};

class FrameTimeProbe {
public:
    // A new presentation generation: the device probe's answer, and no pending interval (a value is
    // only readable after its submission completed, so nothing carries across a device loss).
    void reset(bool timestamps_usable);

    // A submission whose slot completed. The host stores it here and the next observation consumes
    // it; an unusable capability ignores the value entirely, so a backend that reports one anyway
    // cannot make the controller act on it.
    void publish(std::optional<double> gpu_frame_time_ms);

    // Selects the frame-time signal for one presented frame and consumes any pending interval.
    [[nodiscard]] FrameTimeObservation observe(double cpu_frame_time_ms);

    [[nodiscard]] bool capability_usable() const {
        return capability_usable_;
    }
    // Whether an interval is waiting to be consumed.
    [[nodiscard]] bool has_pending() const {
        return pending_gpu_frame_time_ms_.has_value();
    }

private:
    bool capability_usable_ = false;
    // At most two frames old: a timestamp value is only readable after its submission completed, so
    // a frame that observes before a new interval exists falls back to the CPU time and is counted.
    std::optional<double> pending_gpu_frame_time_ms_;
};

} // namespace tessera::app::detail
