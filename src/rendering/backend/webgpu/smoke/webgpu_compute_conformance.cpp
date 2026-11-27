// SPDX-License-Identifier: MIT

// Emscripten-only compute conformance for the WebGPU backend (RHI 1.22, issue #684). It needs a
// real WebGPU device, which the host hands over as `Module.preinitializedWebGPUDevice`: a browser
// page or scripts/webgpu_compute_conformance.mjs (Node with Dawn's `webgpu` bindings). The kernels
// and their exact expected results are the ones of tests/gpu/rhi/compute_conformance_tests.cpp, so
// the prefix-sum, atomic-histogram and indirect-dispatch behavior is the same on every backend.
// Built with JSPI (not ASYNCIFY, which cannot be mixed with the wasm exceptions the lane compiles
// with) so the readback can wait for the buffer map in straight-line code; the host needs a
// JSPI-capable engine.

#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "webgpu_internal.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <emscripten/emscripten.h>
#include <exception>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace webgpu = tessera::rendering::webgpu;

constexpr char kWgsl[] =
#include "compute_conformance_wgsl.inc"
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
        std::fprintf(stderr, "webgpu_compute_conformance: %s\n", message);
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

struct MapState {
    bool done = false;
    bool ok = false;
};

void on_map(WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
    auto* state = static_cast<MapState*>(userdata);
    state->ok = status == WGPUMapAsyncStatus_Success;
    state->done = true;
}

// Waits by yielding to the event loop (ASYNCIFY): the completion and map callbacks fire from JS.
template<typename Ready> bool wait_for(Ready&& ready) {
    for (int spins = 0; spins < 5000 && !ready(); ++spins) {
        emscripten_sleep(1);
    }
    return ready();
}

std::vector<std::uint32_t> read_words(rhi::Buffer& readback, std::uint32_t count) {
    auto& buffer = static_cast<webgpu::WebGpuBuffer&>(readback);
    MapState state;
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = on_map;
    info.userdata1 = &state;
    wgpuBufferMapAsync(buffer.handle(), WGPUMapMode_Read, 0, count * 4U, info);
    std::vector<std::uint32_t> words;
    if (wait_for([&] { return state.done; }) && state.ok) {
        const auto* mapped = static_cast<const std::uint32_t*>(
            wgpuBufferGetConstMappedRange(buffer.handle(), 0, count * 4U));
        words.assign(mapped, mapped + count);
        wgpuBufferUnmap(buffer.handle());
    }
    return words;
}

bool run(rhi::Device& device) {
    if (!expect(device.supports_compute(), "the device reports compute")) {
        return false;
    }
    const rhi::ComputeCapabilities caps = device.compute_capabilities();
    if (!expect(caps.indirect_dispatch && caps.buffer_atomics &&
                    caps.max_workgroup_count == 65535 && caps.max_workgroup_invocations >= kGroup &&
                    caps.max_shared_memory_bytes >= kGroup * 4,
                "the capability report covers the kernels")) {
        return false;
    }
    std::printf("webgpu_compute_conformance: workgroup %ux%ux%u (%u invocations), shared %u B\n",
                caps.max_workgroup_size_x, caps.max_workgroup_size_y, caps.max_workgroup_size_z,
                caps.max_workgroup_invocations, caps.max_shared_memory_bytes);

    rhi::ComputePipelineDesc pipeline_desc;
    pipeline_desc.shader_id = "compute_conformance";
    pipeline_desc.storage_buffer_count = 2;
    pipeline_desc.push_constant_size = sizeof(Push);
    if (!expect(throws([&] { (void)device.create_compute_pipeline({}); }),
                "a pipeline without a shader id is rejected") ||
        !expect(throws([&] {
                    rhi::ComputePipelineDesc unknown = pipeline_desc;
                    unknown.shader_id = "no_such_program";
                    (void)device.create_compute_pipeline(unknown);
                }),
                "an unknown shader id is rejected")) {
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
    const auto in_buffer = device.create_buffer({kCount * 4, rhi::BufferUsage::Storage});
    in_buffer->write(input.data(), kCount * 4);
    const auto scan_in = device.create_buffer({kGroup * 4, rhi::BufferUsage::Storage});
    scan_in->write(scan_input.data(), kGroup * 4);
    const auto args = device.create_buffer({12, rhi::BufferUsage::Storage, true});
    const auto hist = device.create_buffer({kBins * 4, rhi::BufferUsage::Storage, true});
    const auto scan_out = device.create_buffer({kGroup * 4, rhi::BufferUsage::Storage, true});
    const auto args_back = device.create_buffer({12, rhi::BufferUsage::Readback});
    const auto hist_back = device.create_buffer({kBins * 4, rhi::BufferUsage::Readback});
    const auto scan_back = device.create_buffer({kGroup * 4, rhi::BufferUsage::Readback});

    // A pipeline's bindings belong to the pipeline object, so each pairing is its own pipeline.
    const auto arg_pipe = device.create_compute_pipeline(pipeline_desc);
    const auto hist_pipe = device.create_compute_pipeline(pipeline_desc);
    const auto scan_pipe = device.create_compute_pipeline(pipeline_desc);

    const auto list = device.create_command_list();
    const auto push = [&](std::uint32_t mode, std::uint32_t count) {
        const Push value{mode, count};
        list->compute_push_constants(&value, sizeof(value));
    };
    list->bind_compute_pipeline(*arg_pipe);
    list->bind_compute_buffer(0, *args);
    list->bind_compute_buffer(1, *in_buffer);
    push(0, (kCount + kGroup - 1) / kGroup);
    list->dispatch(1, 1, 1);
    list->bind_compute_pipeline(*hist_pipe);
    list->bind_compute_buffer(0, *hist);
    list->bind_compute_buffer(1, *in_buffer);
    push(1, 0);
    list->dispatch(1, 1, 1);
    list->compute_barrier();
    push(2, kCount);
    list->dispatch_indirect(*args, 0);
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
                "a non-Storage buffer throws") ||
        !expect(throws([&] { list->dispatch(0, 1, 1); }), "a zero count throws") ||
        !expect(throws([&] { list->bind_compute_buffer(2, *hist); }),
                "a slot past the pipeline throws")) {
        return false;
    }
    list->submit_async();
    if (!expect(wait_for([&] { return list->is_complete(); }), "the queue finished the work")) {
        return false;
    }

    const auto arguments = read_words(*args_back, 3);
    if (!expect(arguments.size() == 3 && arguments[0] == 4 && arguments[1] == 1 &&
                    arguments[2] == 1,
                "the first pass wrote the indirect arguments")) {
        return false;
    }
    const auto bins = read_words(*hist_back, kBins);
    if (!expect(bins.size() == kBins, "the histogram was read back")) {
        return false;
    }
    for (std::uint32_t bin = 0; bin < kBins; ++bin) {
        const std::uint32_t want = kCount / kBins + (bin < kCount % kBins ? 1U : 0U);
        if (bins[bin] != want) {
            std::fprintf(stderr,
                         "webgpu_compute_conformance: histogram bin %u is %u, expected %u\n", bin,
                         bins[bin], want);
            return false;
        }
    }
    const auto sums = read_words(*scan_back, kScanCount);
    if (!expect(sums.size() == kScanCount, "the prefix sum was read back")) {
        return false;
    }
    for (std::uint32_t i = 0; i < kScanCount; ++i) {
        const std::uint32_t want = i * (i + 1) / 2;
        if (sums[i] != want) {
            std::fprintf(stderr, "webgpu_compute_conformance: prefix sum %u is %u, expected %u\n",
                         i, sums[i], want);
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    try {
        WGPUDevice wgpu_device = emscripten_webgpu_get_device();
        WGPUQueue queue = wgpuDeviceGetQueue(wgpu_device);
        auto device = webgpu::create_device(wgpu_device, queue);
        webgpu::WgslProgram program;
        program.source = kWgsl;
        if (!webgpu::register_shader(*device, "compute_conformance", program)) {
            std::fprintf(stderr, "webgpu_compute_conformance: register_shader failed\n");
            return 1;
        }
        if (!run(*device)) {
            return 2;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "webgpu_compute_conformance: unexpected exception: %s\n",
                     error.what());
        return 3;
    }
    std::puts("webgpu_compute_conformance: ok");
    return 0;
}
