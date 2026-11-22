// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/memory.hpp"

#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// psapi.h must follow windows.h.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tessera::diagnostics {

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)

// Parses "<key>:   <n> kB" out of /proc/self/status text. Returns 0 when the key is absent.
std::uint64_t kilobytes_of(const char* text, std::size_t length, const char* key) noexcept {
    std::size_t key_length = 0;
    while (key[key_length] != '\0') {
        ++key_length;
    }
    for (std::size_t i = 0; i + key_length < length; ++i) {
        if ((i != 0 && text[i - 1] != '\n') || text[i + key_length] != ':') {
            continue;
        }
        std::size_t k = 0;
        while (k < key_length && text[i + k] == key[k]) {
            ++k;
        }
        if (k != key_length) {
            continue;
        }
        std::size_t p = i + key_length + 1;
        while (p < length && (text[p] == ' ' || text[p] == '\t')) {
            ++p;
        }
        std::uint64_t value = 0;
        while (p < length && text[p] >= '0' && text[p] <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(text[p] - '0');
            ++p;
        }
        return value;
    }
    return 0;
}

#endif

} // namespace

ProcessMemory sample_process_memory() noexcept {
    ProcessMemory out;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        out.resident_bytes = counters.WorkingSetSize;
        out.peak_resident_bytes = counters.PeakWorkingSetSize;
        out.valid = true;
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        out.resident_bytes = info.resident_size;
        out.peak_resident_bytes = info.resident_size_max;
        out.valid = true;
    }
#else
    // /proc/self/status is under 2 KiB on Linux and Android; VmHWM and VmRSS sit well inside 4 KiB.
    char buffer[4096];
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t length = 0;
        for (;;) {
            const ssize_t got = ::read(fd, buffer + length, sizeof(buffer) - length);
            if (got <= 0) {
                break;
            }
            length += static_cast<std::size_t>(got);
            if (length == sizeof(buffer)) {
                break;
            }
        }
        ::close(fd);
        const std::uint64_t rss = kilobytes_of(buffer, length, "VmRSS");
        const std::uint64_t hwm = kilobytes_of(buffer, length, "VmHWM");
        if (rss != 0) {
            out.resident_bytes = rss * 1024;
            out.peak_resident_bytes = (hwm != 0 ? hwm : rss) * 1024;
            out.valid = true;
        }
    }
#endif
    if (out.valid && out.peak_resident_bytes < out.resident_bytes) {
        out.peak_resident_bytes = out.resident_bytes;
    }
    return out;
}

MemoryGauges::MemoryGauges(MetricsRegistry& registry)
    : registry_(&registry),
      resident_id_(registry.add_gauge("memory.rss_bytes")),
      peak_id_(registry.add_gauge("memory.rss_peak_bytes")) {}

ProcessMemory MemoryGauges::sample() noexcept {
    const ProcessMemory memory = sample_process_memory();
    if (memory.valid) {
        registry_->set(resident_id_, memory.resident_bytes);
        registry_->set(peak_id_, memory.peak_resident_bytes);
    }
    return memory;
}

namespace {

// The gauge-name stem for each category, in `rhi::MemoryCategory` order (issue #703). A table
// rather than a switch so the names and the enum cannot drift apart silently:
// `kGpuMemoryCategoryCount` is asserted against the table length below.
constexpr const char* kCategoryNames[] = {
    "vertex",  "index",         "uniform",     "staging",    "readback", "storage",
    "texture", "render_target", "glyph_atlas", "path_cache", "other",
};

static_assert(std::size(kCategoryNames) == kGpuMemoryCategoryCount,
              "every MemoryCategory needs a gauge name");

} // namespace

GpuMemoryGauges::GpuMemoryGauges(MetricsRegistry& registry)
    : registry_(&registry),
      ids_{registry.add_gauge("gpu.buffer_bytes"), registry.add_gauge("gpu.image_bytes"),
           registry.add_gauge("gpu.block_bytes"), registry.add_gauge("gpu.allocation_bytes")} {
    // One live and one peak gauge per category. Registration allocates, which is why it happens
    // here and not in `publish`.
    for (std::size_t i = 0; i < kGpuMemoryCategoryCount; ++i) {
        const std::string live = std::string("gpu.category.") + kCategoryNames[i] + ".bytes";
        const std::string peak = std::string("gpu.category.") + kCategoryNames[i] + ".peak_bytes";
        category_ids_[i] = registry.add_gauge(live);
        category_peak_ids_[i] = registry.add_gauge(peak);
    }
}

void GpuMemoryGauges::publish(const GpuMemory& memory) noexcept {
    registry_->set(ids_[0], memory.buffer_bytes);
    registry_->set(ids_[1], memory.image_bytes);
    registry_->set(ids_[2], memory.block_bytes);
    registry_->set(ids_[3], memory.allocation_bytes);
    for (std::size_t i = 0; i < kGpuMemoryCategoryCount; ++i) {
        registry_->set(category_ids_[i], memory.category_bytes[i]);
        registry_->set(category_peak_ids_[i], memory.category_peak_bytes[i]);
    }
}

CacheGauges::CacheGauges(MetricsRegistry& registry, std::string_view name)
    : registry_(&registry),
      name_(name) {
    // One gauge per counter, named after the cache so a diagnostic can group them. Registration
    // allocates, which is why it happens here and not in `publish`.
    const std::string prefix = "cache." + name_ + ".";
    ids_[0] = registry.add_gauge(prefix + "hits");
    ids_[1] = registry.add_gauge(prefix + "misses");
    ids_[2] = registry.add_gauge(prefix + "evictions");
    ids_[3] = registry.add_gauge(prefix + "entries");
    ids_[4] = registry.add_gauge(prefix + "bytes");
}

void CacheGauges::publish(const CacheStats& stats) noexcept {
    registry_->set(ids_[0], stats.hits);
    registry_->set(ids_[1], stats.misses);
    registry_->set(ids_[2], stats.evictions);
    registry_->set(ids_[3], stats.entries);
    registry_->set(ids_[4], stats.bytes);
}

} // namespace tessera::diagnostics
