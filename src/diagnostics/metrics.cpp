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
    : slots_(std::make_unique<Slot[]>(clamp_capacity(capacity))),
      capacity_(clamp_capacity(capacity)) {}

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
    Slot& slot = slots_[count_];
    slot.name.assign(name);
    slot.kind = kind;
    slot.used = true;
    return count_++;
}

void MetricsRegistry::increment(std::size_t id, std::uint64_t amount) noexcept {
    if (valid(id)) {
        slots_[id].value.fetch_add(amount, std::memory_order_relaxed);
    }
}

void MetricsRegistry::set(std::size_t id, std::uint64_t value) noexcept {
    if (valid(id)) {
        slots_[id].value.store(value, std::memory_order_relaxed);
    }
}

std::uint64_t MetricsRegistry::value(std::size_t id) const noexcept {
    return valid(id) ? slots_[id].value.load(std::memory_order_relaxed) : 0;
}

MetricKind MetricsRegistry::kind(std::size_t id) const noexcept {
    return valid(id) ? slots_[id].kind : MetricKind::Counter;
}

std::string_view MetricsRegistry::name(std::size_t id) const noexcept {
    return valid(id) ? std::string_view(slots_[id].name) : std::string_view{};
}

std::size_t MetricsRegistry::size() const noexcept {
    return count_;
}

std::vector<MetricSample> MetricsRegistry::snapshot() const {
    std::vector<MetricSample> samples;
    samples.reserve(count_);
    for (std::size_t id = 0; id < capacity_; ++id) {
        const Slot& slot = slots_[id];
        if (slot.used) {
            samples.push_back(
                MetricSample{slot.name, slot.value.load(std::memory_order_relaxed), slot.kind});
        }
    }
    return samples;
}

void MetricsRegistry::reset() noexcept {
    for (std::size_t id = 0; id < capacity_; ++id) {
        if (slots_[id].used) {
            slots_[id].value.store(0, std::memory_order_relaxed);
        }
    }
}

} // namespace tessera::diagnostics
