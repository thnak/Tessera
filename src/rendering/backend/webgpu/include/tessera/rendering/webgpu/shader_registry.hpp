// SPDX-License-Identifier: MIT

#pragma once

// WGSL lookup for RHI pipelines.
//
// `rhi::PipelineDesc` carries SPIR-V only, and ADR-011 keeps WGSL as a separately authored twin
// (no runtime shader translation). Until the RHI grows a shader-id field, the WebGPU backend
// resolves a pipeline's shaders by the identity of its SPIR-V pointer: the host registers the same
// pointer its `PipelineDesc` will carry together with the WGSL module. In a Wasm build there is no
// glslc output, so any stable word-aligned static object serves as the key.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::rendering::webgpu {

// One WGSL module holding both stages; entry point names are explicit because WGSL allows any.
struct WgslProgram {
    std::string_view source;
    std::string_view vertex_entry = "vs_main";
    std::string_view fragment_entry = "fs_main";
};

class ShaderRegistry {
public:
    // Registers `program` under `key`. A repeated key replaces the earlier program. Registration
    // allocates; do it at startup, never per frame.
    void add(const std::uint32_t* key, const WgslProgram& program);

    // Null when the key was never registered; lookup is allocation-free.
    [[nodiscard]] const WgslProgram* find(const std::uint32_t* key) const;

    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

private:
    struct Entry {
        const std::uint32_t* key = nullptr;
        WgslProgram program;
    };
    std::vector<Entry> entries_;
};

// The built-in batched 2D program (shaders/batched2d.wgsl embedded at configure time).
[[nodiscard]] WgslProgram batched2d_wgsl_program();

} // namespace tessera::rendering::webgpu
