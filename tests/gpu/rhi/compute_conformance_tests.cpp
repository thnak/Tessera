// SPDX-License-Identifier: MIT
//
// RHI 1.18 compute conformance (issue #684): the capability report, an indirect dispatch sized by
// an earlier pass, a buffer-atomic histogram and a shared-memory prefix sum, each with exact
// expected output. The null compute device records the same calls (and moves buffer bytes) so the
// command stream is testable on any machine; Vulkan proves the results. Skips the Vulkan half
// without a device.

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
namespace null = tessera::rendering::null;

alignas(4) const std::uint32_t kConformanceSpv[] =
#include "compute_conformance_comp_spv.h"
    ;

constexpr std::uint32_t kCount = 1000;
constexpr std::uint32_t kBins = 16;
constexpr std::uint32_t kScanCount = 200;
constexpr std::uint32_t kGroup = 256;

struct Push {
    std::uint32_t mode;
    std::uint32_t count;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "compute_conformance: " << message << "\n";
    }
    return condition;
}

template<typename F> bool throws(F&& call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

ComputePipelineDesc conformance_desc() {
    ComputePipelineDesc desc;
    desc.spirv = kConformanceSpv;
    desc.spirv_size = sizeof(kConformanceSpv);
    desc.storage_buffer_count = 2;
    desc.push_constant_size = sizeof(Push);
    return desc;
}

bool check_null_compute() {
    DeviceDesc desc;
    desc.backend = Backend::Null;

    // The default null device is unchanged: no compute, zeroed capabilities, indirect throws.
    const auto plain = null::create_null_device(desc);
    const auto plain_list = plain->create_command_list();
    const ComputeCapabilities none = plain->compute_capabilities();
    const auto storage = plain->create_buffer({64, BufferUsage::Storage});
    if (!expect(!plain->supports_compute() && none.max_workgroup_invocations == 0 &&
                    !none.indirect_dispatch && !none.buffer_atomics,
                "the default null device reports no compute capabilities") ||
        !expect(throws([&] { plain_list->dispatch_indirect(*storage, 0); }),
                "dispatch_indirect throws without compute")) {
        return false;
    }

    const auto device = null::create_null_compute_device(desc);
    const ComputeCapabilities caps = device->compute_capabilities();
    if (!expect(device->supports_compute(), "the null compute device supports compute") ||
        !expect(caps.indirect_dispatch && caps.buffer_atomics &&
                    caps.max_workgroup_count == 65535 && caps.max_workgroup_invocations >= kGroup &&
                    caps.max_shared_memory_bytes >= 1024,
                "the null compute capabilities are the documented fixed set")) {
        return false;
    }
    const auto pipeline = device->create_compute_pipeline(conformance_desc());
    const auto args = device->create_buffer({12, BufferUsage::Storage});
    const auto data = device->create_buffer({64, BufferUsage::Storage});
    const auto readback = device->create_buffer({12, BufferUsage::Readback});
    const std::array<std::uint32_t, 3> counts{4, 2, 1};
    args->write(counts.data(), sizeof(counts));

    auto list_owner = device->create_command_list();
    auto& list = *list_owner;
    if (!expect(throws([&] { list.dispatch(1, 1, 1); }), "dispatch without a pipeline throws") ||
        !expect(throws([&] { (void)device->create_compute_pipeline({}); }),
                "an empty pipeline description is rejected")) {
        return false;
    }
    list.bind_compute_pipeline(*pipeline);
    list.bind_compute_buffer(0, *data);
    list.bind_compute_buffer(1, *args);
    const Push push{2, kCount};
    list.compute_push_constants(&push, sizeof(push));
    list.dispatch(3, 1, 1);
    list.compute_barrier();
    list.dispatch_indirect(*args, 0);
    list.copy_buffer(*args, *readback, 12);
    if (!expect(throws([&] { list.dispatch_indirect(*args, 2); }), "an unaligned offset throws") ||
        !expect(throws([&] { list.dispatch_indirect(*args, 4); }),
                "arguments past the end of the buffer throw") ||
        !expect(throws([&] { list.dispatch_indirect(*readback, 0); }),
                "a non-Storage buffer throws") ||
        !expect(throws([&] { list.dispatch(0, 1, 1); }), "a zero count throws") ||
        !expect(throws([&] { list.bind_compute_buffer(2, *data); }),
                "a slot past the pipeline throws")) {
        return false;
    }
    list.submit();

    const auto& stats = static_cast<const null::NullCommandList&>(list).stats();
    std::array<std::uint32_t, 3> copied{};
    readback->read(copied.data(), sizeof(copied));
    using Kind = null::NullCommandKind;
    const std::vector<Kind> expected_order{
        Kind::BindComputePipeline,  Kind::BindComputeBuffer, Kind::BindComputeBuffer,
        Kind::ComputePushConstants, Kind::Dispatch,          Kind::ComputeBarrier,
        Kind::DispatchIndirect,     Kind::CopyBuffer,        Kind::Submit};
    return expect(stats.command_order == expected_order, "compute commands record in order") &&
           expect(stats.dispatches.size() == 2 && !stats.dispatches[0].indirect &&
                      stats.dispatches[0].x == 3 && stats.dispatches[1].indirect &&
                      stats.dispatches[1].x == 4 && stats.dispatches[1].y == 2 &&
                      stats.dispatches[1].z == 1,
                  "an indirect dispatch records the counts read from the buffer") &&
           expect(stats.compute_barriers == 1 && stats.buffer_copies == 1 &&
                      stats.compute_pipeline_binds == 1 && stats.compute_buffer_binds == 2,
                  "the compute counters match") &&
           expect(copied == counts, "the null copy_buffer moves the bytes");
}

std::vector<std::uint32_t> read_words(const Buffer& buffer, std::size_t count) {
    std::vector<std::uint32_t> words(count);
    buffer.read(words.data(), count * sizeof(std::uint32_t));
    return words;
}

bool check_vulkan(Device& device) {
    const ComputeCapabilities caps = device.compute_capabilities();
    if (!expect(caps.max_workgroup_size_x >= kGroup && caps.max_workgroup_invocations >= kGroup &&
                    caps.max_workgroup_count >= 4 && caps.max_shared_memory_bytes >= 1024 &&
                    caps.indirect_dispatch && caps.buffer_atomics,
                "the device reports the capabilities the kernels need")) {
        return false;
    }

    std::vector<std::uint32_t> input(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        input[i] = i;
    }
    std::vector<std::uint32_t> scan_input(kGroup);
    for (std::uint32_t i = 0; i < kGroup; ++i) {
        scan_input[i] = i + 1;
    }
    const auto in_buffer = device.create_buffer({kCount * 4, BufferUsage::Storage});
    in_buffer->write(input.data(), kCount * 4);
    const auto scan_in = device.create_buffer({kGroup * 4, BufferUsage::Storage});
    scan_in->write(scan_input.data(), kGroup * 4);
    BufferDesc gpu{kGroup * 4, BufferUsage::Storage};
    gpu.gpu_read_write = true;
    const auto args = device.create_buffer({12, BufferUsage::Storage, true});
    const auto hist = device.create_buffer({kBins * 4, BufferUsage::Storage, true});
    const auto scan_out = device.create_buffer(gpu);
    const auto args_back = device.create_buffer({12, BufferUsage::Readback});
    const auto hist_back = device.create_buffer({kBins * 4, BufferUsage::Readback});
    const auto scan_back = device.create_buffer({kGroup * 4, BufferUsage::Readback});

    // A pipeline owns its descriptor set, so each buffer pairing is its own pipeline object.
    const auto arg_pipe = device.create_compute_pipeline(conformance_desc());
    const auto hist_pipe = device.create_compute_pipeline(conformance_desc());
    const auto scan_pipe = device.create_compute_pipeline(conformance_desc());

    const auto list = device.create_command_list();
    const auto push = [&](std::uint32_t mode, std::uint32_t count) {
        const Push value{mode, count};
        list->compute_push_constants(&value, sizeof(value));
    };
    // 1. A one-invocation pass writes the dispatch size of the histogram: ceil(1000 / 256) groups.
    list->bind_compute_pipeline(*arg_pipe);
    list->bind_compute_buffer(0, *args);
    list->bind_compute_buffer(1, *in_buffer);
    push(0, (kCount + kGroup - 1) / kGroup);
    list->dispatch(1, 1, 1);
    // 2. Clear the histogram, then count with buffer atomics under that indirect dispatch.
    list->bind_compute_pipeline(*hist_pipe);
    list->bind_compute_buffer(0, *hist);
    list->bind_compute_buffer(1, *in_buffer);
    push(1, 0);
    list->dispatch(1, 1, 1);
    list->compute_barrier();
    push(2, kCount);
    list->dispatch_indirect(*args, 0);
    // 3. A shared-memory exclusive prefix sum in one workgroup.
    list->bind_compute_pipeline(*scan_pipe);
    list->bind_compute_buffer(0, *scan_out);
    list->bind_compute_buffer(1, *scan_in);
    push(3, kScanCount);
    list->dispatch(1, 1, 1);
    list->compute_barrier();
    list->copy_buffer(*args, *args_back, 12);
    list->copy_buffer(*hist, *hist_back, kBins * 4);
    list->copy_buffer(*scan_out, *scan_back, kGroup * 4);

    if (!expect(throws([&] { list->dispatch_indirect(*in_buffer, 2); }),
                "an unaligned offset throws") ||
        !expect(throws([&] { list->dispatch_indirect(*args, 4); }),
                "arguments past the end throw") ||
        !expect(throws([&] { list->dispatch_indirect(*args_back, 0); }),
                "a non-Storage buffer throws")) {
        return false;
    }
    list->submit();

    const auto arguments = read_words(*args_back, 3);
    if (!expect(arguments[0] == 4 && arguments[1] == 1 && arguments[2] == 1,
                "the first pass wrote the indirect arguments")) {
        return false;
    }
    const auto bins = read_words(*hist_back, kBins);
    for (std::uint32_t bin = 0; bin < kBins; ++bin) {
        const std::uint32_t want = kCount / kBins + (bin < kCount % kBins ? 1U : 0U);
        if (bins[bin] != want) {
            std::cerr << "compute_conformance: histogram bin " << bin << " is " << bins[bin]
                      << ", expected " << want << "\n";
            return false;
        }
    }
    const auto sums = read_words(*scan_back, kScanCount);
    for (std::uint32_t i = 0; i < kScanCount; ++i) {
        const std::uint32_t want = i * (i + 1) / 2;
        if (sums[i] != want) {
            std::cerr << "compute_conformance: prefix sum " << i << " is " << sums[i]
                      << ", expected " << want << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!check_null_compute()) {
        return 1;
    }
    DeviceDesc desc;
    desc.backend = Backend::Vulkan;
    desc.validation = true;
    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(desc);
    } catch (const std::exception& error) {
        std::cout << "compute_conformance: null half ok, Vulkan skipped (device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }
    if (!device->supports_compute()) {
        std::cout << "compute_conformance: null half ok, Vulkan skipped (no compute)\n";
        return 0;
    }
    try {
        if (!check_vulkan(*device)) {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "compute_conformance: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "compute_conformance: ok\n";
    return 0;
}
