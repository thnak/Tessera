// SPDX-License-Identifier: MIT

#pragma once

// WGSL lookup for RHI pipelines.
//
// ADR-049: a pipeline names its program with `rhi::PipelineDesc::shader_id`, and each non-SPIR-V
// backend resolves that id against a table it owns. This registry is the WebGPU table. WGSL is
// authored (ADR-011) and embedded at configure time, so nothing is translated at run time. The
// SPIR-V fields of the descriptor are ignored here; a Wasm build has no glslc output at all.

#include <cstddef>
#include <string>
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
    // Registers `program` under `shader_id`, replacing an earlier program with the same id. An
    // empty id is ignored and returns false (empty means "SPIR-V words" in `PipelineDesc`, which
    // WebGPU cannot consume). Registration allocates; do it at startup, never per frame.
    bool add(std::string_view shader_id, const WgslProgram& program);

    // Null for an empty or unknown id; lookup is allocation-free.
    [[nodiscard]] const WgslProgram* find(std::string_view shader_id) const;

    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

private:
    struct Entry {
        std::string id;
        WgslProgram program;
    };
    std::vector<Entry> entries_;
};

// The built-in batched 2D program (shaders/batched2d.wgsl embedded at configure time).
[[nodiscard]] WgslProgram batched2d_wgsl_program();

// A registry holding every built-in program under its manifest id (`gpu::kBatched2dShaderId`).
// Each WebGPU device starts from this table; hosts add custom programs with `register_shader`.
[[nodiscard]] ShaderRegistry builtin_shader_registry();

} // namespace tessera::rendering::webgpu
