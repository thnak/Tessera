// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/metrics.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::diagnostics {

// Resident memory of this process, as the OS reports it (issue #13). `resident_bytes` is VmRSS on
// Linux/Android, the working set on Windows and `resident_size` on macOS/iOS; the peak is the
// matching high-water mark. A value of 0 with `valid == false` means the platform gave no answer.
struct ProcessMemory {
    std::uint64_t resident_bytes = 0;
    std::uint64_t peak_resident_bytes = 0;
    bool valid = false;
};

// Reads the counters with no heap allocation (a stack buffer and a direct syscall), so it is safe
// on the frame path. It is still a syscall: sample once per second or per scripted checkpoint, not
// per frame.
[[nodiscard]] ProcessMemory sample_process_memory() noexcept;

// Publishes the sample as gauges `memory.rss_bytes` and `memory.rss_peak_bytes` in a registry, so
// the overlay and devtools read them like any other metric. Registration allocates once;
// `sample` does not.
class MemoryGauges {
public:
    explicit MemoryGauges(MetricsRegistry& registry);

    // Returns the sample it published (also for callers that want the numbers directly).
    ProcessMemory sample() noexcept;

    [[nodiscard]] std::size_t resident_id() const noexcept {
        return resident_id_;
    }
    [[nodiscard]] std::size_t peak_id() const noexcept {
        return peak_id_;
    }

private:
    MetricsRegistry* registry_ = nullptr;
    std::size_t resident_id_ = MetricsRegistry::kInvalidMetric;
    std::size_t peak_id_ = MetricsRegistry::kInvalidMetric;
};

} // namespace tessera::diagnostics
