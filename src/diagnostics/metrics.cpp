// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/metrics.hpp"

#include <algorithm>

namespace tessera::diagnostics {
namespace {

std::size_t clamp_capacity(std::size_t capacity) {
    return std::max<std::size_t>(1, std::min(capacity, MetricsRegistry::kMaxMetrics));
}

} // namespace

MetricsRegistry::MetricsRegistry(std::size_t capacity)
    : names_(std::make_unique<SlotName[]>(clamp_capacity(capacity))),
      values_(std::make_unique<std::atomic<std::uint64_t>[]>(clamp_capacity(capacity))),
      capacity_(clamp_capacity(capacity)) {
    // `std::atomic` is not value-initialized by `make_unique<T[]>(n)`, so zero the hot array
    // explicitly; a fresh registry must report 0 for every registered counter.
    for (std::size_t id = 0; id < capacity_; ++id) {
        values_[id].store(0, std::memory_order_relaxed);
    }
}

std::size_t MetricsRegistry::add_counter(std::string_view name) {
    return add(name, MetricKind::Counter);
}

std::size_t MetricsRegistry::add_gauge(std::string_view name) {
    return add(name, MetricKind::Gauge);
}

std::size_t MetricsRegistry::add(std::string_view name, MetricKind kind) {
    if (name.empty() || count_ >= capacity_) {
        return kInvalidMetric;
    }
    SlotName& slot = names_[count_];
    slot.name.assign(name);
    slot.kind = kind;
    slot.used = true;
    values_[count_].store(0, std::memory_order_relaxed);
    return count_++;
}

void MetricsRegistry::increment(std::size_t id, std::uint64_t amount) noexcept {
    if (valid(id)) {
        values_[id].fetch_add(amount, std::memory_order_relaxed);
    }
}

void MetricsRegistry::set(std::size_t id, std::uint64_t value) noexcept {
    if (valid(id)) {
        values_[id].store(value, std::memory_order_relaxed);
    }
}

std::uint64_t MetricsRegistry::value(std::size_t id) const noexcept {
    return valid(id) ? values_[id].load(std::memory_order_relaxed) : 0;
}

MetricKind MetricsRegistry::kind(std::size_t id) const noexcept {
    return valid(id) ? names_[id].kind : MetricKind::Counter;
}

std::string_view MetricsRegistry::name(std::size_t id) const noexcept {
    return valid(id) ? std::string_view(names_[id].name) : std::string_view{};
}

std::size_t MetricsRegistry::size() const noexcept {
    return count_;
}

std::vector<MetricSample> MetricsRegistry::snapshot() const {
    std::vector<MetricSample> samples;
    samples.reserve(count_);
    for (std::size_t id = 0; id < capacity_; ++id) {
        const SlotName& slot = names_[id];
        if (slot.used) {
            samples.push_back(
                MetricSample{slot.name, values_[id].load(std::memory_order_relaxed), slot.kind});
        }
    }
    return samples;
}

void MetricsRegistry::reset() noexcept {
    for (std::size_t id = 0; id < capacity_; ++id) {
        if (names_[id].used) {
            values_[id].store(0, std::memory_order_relaxed);
        }
    }
}

} // namespace tessera::diagnostics
