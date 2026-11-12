// SPDX-License-Identifier: MIT

// Emscripten-only link and load smoke for the WebGPU backend. It runs under Node, which has no
// WebGPU, so it exercises only paths that never reach the browser API: the device entry point's
// argument check, the built-in WGSL registry and the mapping tables. Linking it pulls every
// webgpu.h symbol the backend references, so an unresolved import fails the build.

#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#include <cstdio>
#include <stdexcept>

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "webgpu_wasm_smoke: FAIL %s\n", what);
    return 1;
}

} // namespace

int main() {
    namespace webgpu = tessera::rendering::webgpu;
    try {
        (void)webgpu::create_device(nullptr, nullptr);
        return fail("create_device accepted null handles");
    } catch (const std::invalid_argument&) {
        // Expected: the host must hand over a ready device and queue.
    }

    const webgpu::ShaderRegistry registry = webgpu::builtin_shader_registry();
    const webgpu::WgslProgram* program = registry.find("batched2d");
    if (program == nullptr || program->source.find("fn vs_main") == std::string_view::npos) {
        return fail("built-in batched2d WGSL missing");
    }
    if (registry.find("") != nullptr || registry.find("unknown") != nullptr) {
        return fail("registry resolved an empty or unknown id");
    }
    if (!webgpu::is_supported_sample_count(4) || webgpu::is_supported_sample_count(2)) {
        return fail("sample count table");
    }

    std::puts("webgpu_wasm_smoke: ok");
    return 0;
}
