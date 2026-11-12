// SPDX-License-Identifier: MIT
//
// Which frame-time signal the adaptive-quality controller sees (docs/design/performance.md section
// 6.1). The rule is pure and lives here so it is testable with a fake capability and no device:
// a GPU sample produced by the RHI timestamp probe wins; otherwise the CPU frame time is used.
// The availability decision itself is the RHI probe's (`GpuTimestampCapability::usable()`); this
// header only selects between two already-produced numbers.

#pragma once

#include "tessera/runtime/diagnostics.hpp"

#include <cmath>
#include <optional>

namespace tessera::runtime {

struct FrameTimeSample {
    double frame_time_ms = 0.0;
    FrameTimeSource source = FrameTimeSource::Cpu;
};

// `gpu_frame_time_ms` is present only when the device reported usable GPU timestamps and the
// completed submission's interval read back as a plausible value. A missing, non-finite or
// non-positive value keeps the CPU sample, which is the default path (including the null backend).
[[nodiscard]] inline FrameTimeSample
select_frame_time(double cpu_frame_time_ms, std::optional<double> gpu_frame_time_ms) noexcept {
    if (gpu_frame_time_ms && std::isfinite(*gpu_frame_time_ms) && *gpu_frame_time_ms > 0.0) {
        return FrameTimeSample{*gpu_frame_time_ms, FrameTimeSource::Gpu};
    }
    return FrameTimeSample{cpu_frame_time_ms, FrameTimeSource::Cpu};
}

} // namespace tessera::runtime
