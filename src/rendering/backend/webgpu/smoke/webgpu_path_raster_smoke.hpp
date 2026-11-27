// SPDX-License-Identifier: MIT

// Emscripten-only: the measurement half of the path rasterizer smoke (issue #685).

#pragma once

#include "tessera/rendering/rhi/device.hpp"

namespace tessera::path_smoke {

struct MeasureOptions {
    bool enabled = false;
    bool dense = false;
    int frames = 120;
    int blobs = 60;
};

// The animated 4K frame of tests/gpu/batched2d/gpu_path_animated_measure.cpp on the WebGPU device:
// prints CPU enqueue + flush time, time until the GPU finished and bytes written per frame.
// Returns the process exit code.
// `wgpu_queue` is the device's WGPUQueue, waited on for the GPU time.
int run_measure(rendering::rhi::Device& device, void* wgpu_queue, const MeasureOptions& options);

} // namespace tessera::path_smoke
