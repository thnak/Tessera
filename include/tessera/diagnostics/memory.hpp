// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/metrics.hpp"

#include <array>
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

// GPU memory as plain numbers, so diagnostics needs no RHI types: the caller copies them from
// rhi::DeviceMemoryStats (see docs/design/performance.md, section 3.1).
//
// `category_bytes` and `category_peak_bytes` (issue #703) are the per-purpose split, indexed by
// `rhi::MemoryCategory`. They are plain arrays rather than an RHI enum so this header stays free of
// rendering types; the caller fills them in the same order the RHI enum declares, and
// `kGpuMemoryCategoryCount` is the length both sides agree on.
inline constexpr std::size_t kGpuMemoryCategoryCount = 11;

struct GpuMemory {
    std::uint64_t buffer_bytes = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t allocation_bytes = 0;
    std::array<std::uint64_t, kGpuMemoryCategoryCount> category_bytes{};
    std::array<std::uint64_t, kGpuMemoryCategoryCount> category_peak_bytes{};
};

// Gauges `gpu.buffer_bytes`, `gpu.image_bytes`, `gpu.block_bytes` and `gpu.allocation_bytes`, plus
// one `gpu.category.<name>.bytes` and `gpu.category.<name>.peak_bytes` pair per category (issue
// #703). Registration allocates once; `publish` does not.
class GpuMemoryGauges {
public:
    explicit GpuMemoryGauges(MetricsRegistry& registry);

    void publish(const GpuMemory& memory) noexcept;

    [[nodiscard]] std::size_t buffer_id() const noexcept {
        return ids_[0];
    }
    [[nodiscard]] std::size_t image_id() const noexcept {
        return ids_[1];
    }
    [[nodiscard]] std::size_t block_id() const noexcept {
        return ids_[2];
    }
    [[nodiscard]] std::size_t allocation_id() const noexcept {
        return ids_[3];
    }
    // The live-bytes gauge for one category, or `kInvalidMetric` for an out-of-range index.
    [[nodiscard]] std::size_t category_id(std::size_t category) const noexcept {
        return category < kGpuMemoryCategoryCount ? category_ids_[category]
                                                  : MetricsRegistry::kInvalidMetric;
    }
    // The peak gauge for one category, or `kInvalidMetric` for an out-of-range index.
    [[nodiscard]] std::size_t category_peak_id(std::size_t category) const noexcept {
        return category < kGpuMemoryCategoryCount ? category_peak_ids_[category]
                                                  : MetricsRegistry::kInvalidMetric;
    }

private:
    MetricsRegistry* registry_ = nullptr;
    std::size_t ids_[4] = {MetricsRegistry::kInvalidMetric, MetricsRegistry::kInvalidMetric,
                           MetricsRegistry::kInvalidMetric, MetricsRegistry::kInvalidMetric};
    std::array<std::size_t, kGpuMemoryCategoryCount> category_ids_{};
    std::array<std::size_t, kGpuMemoryCategoryCount> category_peak_ids_{};
};

} // namespace tessera::diagnostics
