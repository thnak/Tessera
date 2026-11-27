// SPDX-License-Identifier: MIT

// Emscripten-only helpers shared by the path rasterizer smoke programs (issue #685): waiting for
// the GPU by yielding to the event loop (JSPI) and the asynchronous buffer readback WebGPU has
// instead of a blocking one.

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "webgpu_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <emscripten/emscripten.h>
#include <vector>

namespace tessera::path_smoke {

namespace rhi = tessera::rendering::rhi;
namespace webgpu = tessera::rendering::webgpu;

struct MapState {
    bool done = false;
    bool ok = false;
};

inline void on_map(WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
    auto* state = static_cast<MapState*>(userdata);
    state->ok = status == WGPUMapAsyncStatus_Success;
    state->done = true;
}

// The completion and map callbacks fire from the JS event loop, so waiting means yielding to it.
template<typename Ready> bool wait_for(Ready&& ready, int max_spins = 120000) {
    for (int spins = 0; spins < max_spins && !ready(); ++spins) {
        emscripten_sleep(0);
    }
    return ready();
}

// Maps `readback` (a rhi::BufferUsage::Readback buffer a submission already copied into; wait for
// that submission first) and returns its first `size` bytes, empty on failure.
inline std::vector<std::uint8_t> read_bytes(rhi::Buffer& readback, std::size_t size) {
    auto& buffer = static_cast<webgpu::WebGpuBuffer&>(readback);
    MapState state;
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = on_map;
    info.userdata1 = &state;
    wgpuBufferMapAsync(buffer.handle(), WGPUMapMode_Read, 0, size, info);
    std::vector<std::uint8_t> bytes;
    if (wait_for([&] { return state.done; }) && state.ok) {
        const auto* mapped = static_cast<const std::uint8_t*>(
            wgpuBufferGetConstMappedRange(buffer.handle(), 0, size));
        bytes.assign(mapped, mapped + size);
        wgpuBufferUnmap(buffer.handle());
    }
    return bytes;
}

} // namespace tessera::path_smoke
