// SPDX-License-Identifier: MIT
//
// GPU timestamp capability and interval arithmetic (docs/design/performance.md section 6.1). This
// is the only place the frame-time signal may learn whether GPU timestamps are usable: the device
// probe fills `GpuTimestampCapability` and nothing above the RHI re-derives the answer. The
// interval helpers are pure so the wraparound/absurdity rules are testable without a device.

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace tessera::rendering::rhi {

// A timestamp interval longer than this is not a frame: it is a stale, wrapped or bogus value, and
// the caller must fall back to the CPU sample instead of feeding it to the quality controller.
inline constexpr double kMaxPlausibleTimestampIntervalMs = 10'000.0;

// The device's answer to "can this device produce a GPU timestamp interval for a submission?".
// All three facts come from the RHI probe (Vulkan: the selected queue family's timestampValidBits,
// that family's graphics capability, and VkPhysicalDeviceLimits::timestampPeriod). A backend with
// no GPU leaves every field at its default, which is the documented CPU fallback.
struct GpuTimestampCapability {
    // Timestamp queries are supported on the queue family this device submits frames on
    // (VkQueueFamilyProperties::timestampValidBits != 0).
    bool queries_supported = false;
    // That queue family can execute the graphics work the timestamp brackets
    // (VK_QUEUE_GRAPHICS_BIT); a timestamp-only family would not measure this frame.
    bool queue_supported = false;
    // Nanoseconds per timestamp tick (VkPhysicalDeviceLimits::timestampPeriod). Zero or
    // non-finite means the device reports no usable period.
    double period_ns = 0.0;

    [[nodiscard]] bool usable() const noexcept {
        return queries_supported && queue_supported && std::isfinite(period_ns) && period_ns > 0.0;
    }
};

// Tick delta between two 64-bit timestamp values. A GPU counter is monotonic, so end < begin can
// only be a wrap of the full 64-bit range; the unsigned subtraction is defined modulo 2^64 and
// therefore computes exactly the wrapped distance. An equal pair (no measured interval) has no
// delta.
[[nodiscard]] inline std::optional<std::uint64_t>
timestamp_delta_ticks(std::uint64_t begin, std::uint64_t end) noexcept {
    if (begin == end) {
        return std::nullopt;
    }
    if (end > begin) {
        return end - begin;
    }
    // begin > end: the counter wrapped once. Defined modulo 2^64 by unsigned arithmetic.
    return end - begin;
}

// The interval in milliseconds, or nullopt when `period_ns` is not a usable period, the delta is
// zero, or the result is absurd (a stale/wrapped pair rather than a frame interval).
[[nodiscard]] inline std::optional<double>
timestamp_interval_ms(std::uint64_t begin, std::uint64_t end, double period_ns) noexcept {
    if (!std::isfinite(period_ns) || period_ns <= 0.0) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> ticks = timestamp_delta_ticks(begin, end);
    if (!ticks) {
        return std::nullopt;
    }
    const double milliseconds = static_cast<double>(*ticks) * period_ns / 1'000'000.0;
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0 ||
        milliseconds > kMaxPlausibleTimestampIntervalMs) {
        return std::nullopt;
    }
    return milliseconds;
}

} // namespace tessera::rendering::rhi
