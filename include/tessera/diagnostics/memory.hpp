// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/metrics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

// The two snapshots as text, one `key=value` line each, for a CLI flag or a log: `process`
// (resident and peak, MB), `gpu` (live buffer/image bytes, allocator blocks and allocations, MB)
// and one `gpu.<category>` line per category with live bytes above zero (KB, live and peak). The
// blocks line is the one to read first: blocks far above allocations are pinned system memory
// nobody uses. Allocates (it builds a string); call it at a checkpoint, never per frame.
[[nodiscard]] std::string format_memory_report(const ProcessMemory& process, const GpuMemory& gpu);

// One cache's counters, in the one shape every cache reports (issue #706, memory-model-v1 section
// 9: "cache hit/miss/eviction/size"). The caches in the tree each grew their own `Stats` struct
// with overlapping-but-not-identical fields; this is the common projection, so a diagnostic can
// iterate caches instead of knowing each one's type.
//
// `bytes` is 0 for a cache that counts entries rather than bytes (the measure cache), and `entries`
// is 0 for one that only tracks bytes. A zero is "not tracked", not "empty": the two are different
// claims and a reader that conflates them will misread a healthy cache as an empty one.
struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
};

// Publishes one cache's counters as gauges `cache.<name>.{hits,misses,evictions,entries,bytes}`.
// Registration allocates once; `publish` does not. The name is the cache's stable identifier
// ("image_texture", "path_texture", "paint", "glyph_atlas", "text_measure"), the same names the
// memory-pressure order uses, so a diagnostic and an eviction log line refer to the same thing.
class CacheGauges {
public:
    CacheGauges(MetricsRegistry& registry, std::string_view name);

    void publish(const CacheStats& stats) noexcept;

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }
    [[nodiscard]] std::size_t hits_id() const noexcept {
        return ids_[0];
    }
    [[nodiscard]] std::size_t misses_id() const noexcept {
        return ids_[1];
    }
    [[nodiscard]] std::size_t evictions_id() const noexcept {
        return ids_[2];
    }
    [[nodiscard]] std::size_t entries_id() const noexcept {
        return ids_[3];
    }
    [[nodiscard]] std::size_t bytes_id() const noexcept {
        return ids_[4];
    }

private:
    MetricsRegistry* registry_ = nullptr;
    std::string name_;
    std::size_t ids_[5] = {MetricsRegistry::kInvalidMetric, MetricsRegistry::kInvalidMetric,
                           MetricsRegistry::kInvalidMetric, MetricsRegistry::kInvalidMetric,
                           MetricsRegistry::kInvalidMetric};
};

} // namespace tessera::diagnostics
