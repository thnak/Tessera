// SPDX-License-Identifier: MIT
//
// Buffer::write / Buffer::read must reject ranges outside the buffer instead of silently dropping
// the call. The same contract is checked on the null backend and, when a device is available, on
// the Vulkan backend.

#include "tessera/rendering/null/null_device.hpp"
#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

using namespace tessera::rendering;

constexpr std::size_t kBufferSize = 16;

bool expect(bool condition, const char* backend, const char* message) {
    if (!condition) {
        std::cerr << "rhi_buffer_bounds[" << backend << "]: " << message << "\n";
    }
    return condition;
}

template<typename Fn> bool throws_out_of_range(Fn&& fn) {
    try {
        fn();
    } catch (const std::out_of_range&) {
        return true;
    }
    return false;
}

bool check_device(rhi::Device& device, const char* backend) {
    // Readback buffers are host-visible on every backend and are valid for write and read.
    auto buffer = device.create_buffer({kBufferSize, rhi::BufferUsage::Readback});
    std::array<std::uint8_t, kBufferSize + 1> data{};
    data.fill(0xAB);
    std::array<std::uint8_t, kBufferSize + 1> out{};

    const std::size_t max = std::numeric_limits<std::size_t>::max();

    // In-range accesses still work, including the exact fit and an empty range at the end.
    buffer->write(data.data(), kBufferSize, 0);
    buffer->read(out.data(), kBufferSize, 0);
    if (!expect(out[0] == 0xAB && out[kBufferSize - 1] == 0xAB, backend, "exact fit round-trips")) {
        return false;
    }
    buffer->write(data.data(), 0, kBufferSize);
    buffer->read(out.data(), 0, kBufferSize);

    return expect(throws_out_of_range([&] { buffer->write(data.data(), kBufferSize + 1, 0); }),
                  backend, "write larger than the buffer throws") &&
           expect(throws_out_of_range([&] { buffer->write(data.data(), 8, 9); }), backend,
                  "write running past the end throws") &&
           expect(throws_out_of_range([&] { buffer->write(data.data(), 1, kBufferSize); }), backend,
                  "write starting at the end throws") &&
           expect(throws_out_of_range([&] { buffer->write(data.data(), 8, max - 3); }), backend,
                  "offset + size overflow is rejected, not wrapped") &&
           expect(throws_out_of_range([&] { buffer->read(out.data(), kBufferSize + 1, 0); }),
                  backend, "read larger than the buffer throws") &&
           expect(throws_out_of_range([&] { buffer->read(out.data(), 8, 9); }), backend,
                  "read running past the end throws") &&
           expect(throws_out_of_range([&] { buffer->read(out.data(), 8, max - 3); }), backend,
                  "read offset overflow is rejected");
}

} // namespace

int main() {
    auto null_device = null::create_null_device(rhi::DeviceDesc{});
    if (!check_device(*null_device, "null")) {
        return 1;
    }

    {
        const auto stats = null_device->memory_statistics();
        if (!expect(stats.allocation_bytes == 0 && stats.block_count == 0, "null",
                    "a backend without tracking reports zero memory")) {
            return 1;
        }
    }

#if defined(TESSERA_TEST_HAS_VULKAN)
    rhi::DeviceDesc vulkan_desc;
    vulkan_desc.backend = rhi::Backend::Vulkan;
    vulkan_desc.validation = false;
    std::unique_ptr<rhi::Device> vulkan_device;
    try {
        vulkan_device = vulkan::create_vulkan_device(vulkan_desc);
    } catch (const std::exception& error) {
        std::cout << "rhi_buffer_bounds: vulkan skipped (" << error.what() << ")\n";
        return 0;
    }
    if (!check_device(*vulkan_device, "vulkan")) {
        return 2;
    }
    {
        const auto caps = vulkan_device->capabilities();
        if (caps.backend != rhi::Backend::Vulkan || caps.rhi_version != rhi::kRhiVersion) {
            std::cerr << "rhi_buffer_bounds[vulkan]: unexpected device capabilities\n";
            return 2;
        }
    }
    {
        constexpr std::size_t kMemoryProbe = 1U << 20;
        const auto before = vulkan_device->memory_statistics();
        auto probe = vulkan_device->create_buffer({kMemoryProbe, rhi::BufferUsage::Readback});
        const auto during = vulkan_device->memory_statistics();
        probe.reset();
        const auto after = vulkan_device->memory_statistics();
        if (!expect(during.allocation_bytes >= before.allocation_bytes + kMemoryProbe, "vulkan",
                    "a live buffer shows in allocation_bytes") ||
            !expect(during.block_bytes >= during.allocation_bytes, "vulkan",
                    "blocks cover allocations") ||
            !expect(after.allocation_bytes == before.allocation_bytes, "vulkan",
                    "freeing the buffer returns allocation_bytes to the baseline")) {
            return 3;
        }
    }
#endif

    std::cout << "rhi_buffer_bounds: ok\n";
    return 0;
}
