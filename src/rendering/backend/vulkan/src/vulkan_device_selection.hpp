// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tessera::rendering::vulkan {

// Plain-data view of a physical device, so the selection policy can be tested without a GPU.
enum class DeviceKind : std::uint8_t { Discrete, Integrated, Virtual, Cpu, Other };

struct DeviceCandidate {
    DeviceKind kind = DeviceKind::Other;
    std::uint32_t api_version = 0; // VK_MAKE_API_VERSION encoding
};

// VK_API_VERSION_1_3 without pulling in the Vulkan headers.
inline constexpr std::uint32_t kMinimumApiVersion = (1U << 22) | (3U << 12);

namespace detail {

[[nodiscard]] constexpr int kind_rank(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Discrete:
        return 4;
    case DeviceKind::Integrated:
        return 3;
    case DeviceKind::Virtual:
        return 2;
    case DeviceKind::Cpu:
        return 1;
    case DeviceKind::Other:
        break;
    }
    return 0;
}

} // namespace detail

// Picks the best device Tessera can run on: only Vulkan 1.3+ devices qualify; among those a
// discrete GPU beats an integrated one, which beats virtual and CPU (software) devices; at equal
// kind the newer API version wins, so a current driver is preferred over a stale one that
// enumerates the same card; remaining ties keep the earliest index (stable across runs).
// Returns nullopt when no device qualifies.
[[nodiscard]] constexpr std::optional<std::size_t>
choose_device(std::span<const DeviceCandidate> candidates) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const DeviceCandidate& candidate = candidates[i];
        if (candidate.api_version < kMinimumApiVersion) {
            continue;
        }
        if (!best) {
            best = i;
            continue;
        }
        const DeviceCandidate& current = candidates[*best];
        const int rank = detail::kind_rank(candidate.kind);
        const int current_rank = detail::kind_rank(current.kind);
        if (rank > current_rank ||
            (rank == current_rank && candidate.api_version > current.api_version)) {
            best = i;
        }
    }
    return best;
}

} // namespace tessera::rendering::vulkan
