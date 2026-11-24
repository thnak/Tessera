// SPDX-License-Identifier: MIT
//
// RHI 1.16 compute (issue #683): a dispatch writes a device-memory storage buffer, a barrier orders a
// second dispatch after it, a buffer copy reads the result back, and every misuse is rejected
// instead of reaching the driver. The null backend answers supports_compute() false and its
// create_compute_pipeline throws. Skips without a Vulkan device.

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace tessera::rendering::rhi;

alignas(4) const std::uint32_t kProbeSpv[] =
#include "compute_probe_comp_spv.h"
    ;

constexpr std::uint32_t kCount = 1000; // not a multiple of the 64-wide workgroup on purpose

struct Push {
    std::uint32_t mul;
    std::uint32_t count;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "compute_dispatch: " << message << "\n";
    }
    return condition;
}

template <typename F> bool throws(F&& call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool check_null_backend() {
    DeviceDesc desc;
    desc.backend = Backend::Null;
    const auto device = tessera::rendering::null::create_null_device(desc);
    ComputePipelineDesc compute;
    compute.spirv = kProbeSpv;
    compute.spirv_size = sizeof(kProbeSpv);
    compute.storage_buffer_count = 2;
    compute.push_constant_size = sizeof(Push);
    const auto list = device->create_command_list();
    return expect(!device->supports_compute(), "the null backend has no compute") &&
           expect(throws([&] { (void)device->create_compute_pipeline(compute); }),
                  "create_compute_pipeline throws without the capability") &&
           expect(throws([&] { list->dispatch(1, 1, 1); }), "dispatch throws without the capability");
}

bool check_vulkan(Device& device) {
    ComputePipelineDesc compute;
    compute.spirv = kProbeSpv;
    compute.spirv_size = sizeof(kProbeSpv);
    compute.storage_buffer_count = 2;
    compute.push_constant_size = sizeof(Push);
    const auto pipeline = device.create_compute_pipeline(compute);

    std::vector<std::uint32_t> input(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        input[i] = i;
    }
    const std::size_t bytes = kCount * sizeof(std::uint32_t);
    const auto in_buffer = device.create_buffer({bytes, BufferUsage::Storage});
    in_buffer->write(input.data(), bytes);
    BufferDesc out_desc{bytes, BufferUsage::Storage};
    out_desc.gpu_read_write = true;
    const auto out_buffer = device.create_buffer(out_desc);
    const auto readback = device.create_buffer({bytes, BufferUsage::Readback});

    if (!expect(throws([&] { std::uint32_t word = 0; out_buffer->read(&word, 4); }),
                "a gpu_read_write buffer is not host-readable")) {
        return false;
    }

    const auto list = device.create_command_list();
    // Misuse is rejected before any command that needs a pipeline.
    if (!expect(throws([&] { list->dispatch(1, 1, 1); }), "dispatch without a pipeline throws") ||
        !expect(throws([&] { list->bind_compute_buffer(0, *out_buffer); }),
                "binding a buffer without a pipeline throws")) {
        return false;
    }
    list->bind_compute_pipeline(*pipeline);
    if (!expect(throws([&] { list->bind_compute_buffer(2, *out_buffer); }),
                "a slot past the pipeline's buffers throws") ||
        !expect(throws([&] { list->dispatch(0, 1, 1); }), "a zero workgroup count throws") ||
        !expect(throws([&] { list->dispatch(70000, 1, 1); }), "an oversized count throws") ||
        !expect(throws([&] { list->copy_buffer(*in_buffer, *readback, bytes + 4); }),
                "a copy larger than the buffers throws")) {
        return false;
    }

    list->bind_compute_buffer(0, *out_buffer);
    list->bind_compute_buffer(1, *in_buffer);
    Push push{3, kCount};
    list->compute_push_constants(&push, sizeof(push));
    list->dispatch((kCount + 63) / 64, 1, 1);
    // The second dispatch overwrites the first, so a missing barrier would be a write-after-write
    // hazard the validation layer reports; the result proves the order.
    list->compute_barrier();
    push.mul = 5;
    list->compute_push_constants(&push, sizeof(push));
    list->dispatch((kCount + 63) / 64, 1, 1);
    list->copy_buffer(*out_buffer, *readback, bytes);
    list->submit();

    std::vector<std::uint32_t> result(kCount);
    readback->read(result.data(), bytes);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        if (result[i] != i * 5 + 1) {
            std::cerr << "compute_dispatch: element " << i << " is " << result[i] << ", expected "
                      << i * 5 + 1 << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!check_null_backend()) {
        return 1;
    }
    DeviceDesc desc;
    desc.backend = Backend::Vulkan;
    desc.validation = true;
    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(desc);
    } catch (const std::exception& error) {
        std::cout << "compute_dispatch: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!device->supports_compute()) {
        std::cout << "compute_dispatch: skipped (the device reports no compute)\n";
        return 0;
    }
    try {
        if (!check_vulkan(*device)) {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "compute_dispatch: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "compute_dispatch: ok\n";
    return 0;
}
