// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::diagnostics {

enum class MetricKind : std::uint8_t { Counter, Gauge };

struct MetricSample {
    std::string_view name;
    std::uint64_t value = 0;
    MetricKind kind = MetricKind::Counter;
};

// Fixed-capacity counter/gauge registry. Registration allocates a slot name once; increments are
// a single relaxed atomic add, so frame/task hot paths never allocate or block. Snapshots copy
// values in registration order and are meant for diagnostics, overlay, and tests.
//
// The hot and cold halves of a slot are stored in two parallel arrays (security review finding
// C3). Interleaved, each slot was a 32-byte `std::string` name followed by the 8-byte counter, so
// the counters sat at a 48-byte stride - 1.33 counters per 64-byte cache line - and every
// `increment` dragged in a line holding a name it never reads. Split, the values are a dense
// `std::atomic<std::uint64_t>` array at 8 counters per line, which is what "cheap to increment
// concurrently" requires.
class MetricsRegistry {
public:
    static constexpr std::size_t kMaxMetrics = 128;
    static constexpr std::size_t kInvalidMetric = static_cast<std::size_t>(-1);

    explicit MetricsRegistry(std::size_t capacity = kMaxMetrics);

    [[nodiscard]] std::size_t add_counter(std::string_view name);
    [[nodiscard]] std::size_t add_gauge(std::string_view name);

    void increment(std::size_t id, std::uint64_t amount = 1) noexcept;
    void set(std::size_t id, std::uint64_t value) noexcept;

    [[nodiscard]] std::uint64_t value(std::size_t id) const noexcept;
    [[nodiscard]] MetricKind kind(std::size_t id) const noexcept;
    [[nodiscard]] std::string_view name(std::size_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::vector<MetricSample> snapshot() const;
    void reset() noexcept;

private:
    // The cold half: written once at registration, read by `name`/`kind`/`snapshot`.
    struct SlotName {
        std::string name;
        MetricKind kind = MetricKind::Counter;
        bool used = false;
    };

    [[nodiscard]] std::size_t add(std::string_view name, MetricKind kind);
    [[nodiscard]] bool valid(std::size_t id) const noexcept {
        return id < capacity_ && names_[id].used;
    }

    std::unique_ptr<SlotName[]> names_;
    // The hot half: one relaxed atomic per slot, contiguous so a cache line holds eight counters.
    std::unique_ptr<std::atomic<std::uint64_t>[]> values_;
    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
};

} // namespace tessera::diagnostics
