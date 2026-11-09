// SPDX-License-Identifier: MIT
//
// Physical-device selection policy, tested on plain data (no GPU needed): only Vulkan 1.3+ devices
// qualify, device kind outranks API version, and a newer driver wins over a stale one for the
// same kind. The stale-driver case mirrors a machine where an old ICD and the current driver both
// enumerate the same card.

#include "vulkan_device_selection.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {

using tessera::rendering::vulkan::choose_device;
using tessera::rendering::vulkan::DeviceCandidate;
using tessera::rendering::vulkan::DeviceKind;

constexpr std::uint32_t version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

int failures = 0;

void expect(const char* name, std::optional<std::size_t> actual,
            std::optional<std::size_t> expected) {
    if (actual != expected) {
        std::cerr << "FAIL " << name << ": expected "
                  << (expected ? static_cast<long long>(*expected) : -1LL) << ", got "
                  << (actual ? static_cast<long long>(*actual) : -1LL) << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    {
        const std::array<DeviceCandidate, 0> none{};
        expect("empty", choose_device(none), std::nullopt);
    }
    {
        // Old ICD listed first, current driver second, same discrete card.
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Discrete, version(1, 3, 217)},
            {DeviceKind::Discrete, version(1, 4, 315)},
        }};
        expect("newer driver beats stale one listed first", choose_device(devices), 1);
    }
    {
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Integrated, version(1, 4, 0)},
            {DeviceKind::Discrete, version(1, 3, 0)},
        }};
        expect("discrete beats a newer integrated device", choose_device(devices), 1);
    }
    {
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Discrete, version(1, 2, 0)},
            {DeviceKind::Integrated, version(1, 3, 0)},
        }};
        expect("a discrete device below 1.3 is skipped", choose_device(devices), 1);
    }
    {
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Discrete, version(1, 2, 0)},
            {DeviceKind::Cpu, version(1, 1, 0)},
        }};
        expect("no 1.3 device", choose_device(devices), std::nullopt);
    }
    {
        // Software rasterizer (lavapipe) is the fallback when there is no GPU.
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Cpu, version(1, 3, 0)},
            {DeviceKind::Other, version(1, 3, 0)},
        }};
        expect("cpu beats other", choose_device(devices), 0);
    }
    {
        const std::array<DeviceCandidate, 2> devices = {{
            {DeviceKind::Discrete, version(1, 3, 0)},
            {DeviceKind::Discrete, version(1, 3, 0)},
        }};
        expect("ties keep the first index", choose_device(devices), 0);
    }
    {
        const std::array<DeviceCandidate, 3> devices = {{
            {DeviceKind::Cpu, version(1, 3, 0)},
            {DeviceKind::Virtual, version(1, 3, 0)},
            {DeviceKind::Integrated, version(1, 3, 0)},
        }};
        expect("integrated beats virtual and cpu", choose_device(devices), 2);
    }

    if (failures == 0) {
        std::cout << "vulkan_device_selection: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
