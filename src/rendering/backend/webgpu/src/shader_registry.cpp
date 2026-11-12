// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"

namespace {

constexpr char kBatched2dWgsl[] =
#include "batched2d_wgsl.inc"
    ;

} // namespace

namespace tessera::rendering::webgpu {

bool ShaderRegistry::add(std::string_view shader_id, const WgslProgram& program) {
    if (shader_id.empty()) {
        return false;
    }
    for (Entry& entry : entries_) {
        if (entry.id == shader_id) {
            entry.program = program;
            return true;
        }
    }
    entries_.push_back(Entry{std::string(shader_id), program});
    return true;
}

const WgslProgram* ShaderRegistry::find(std::string_view shader_id) const {
    if (shader_id.empty()) {
        return nullptr;
    }
    for (const Entry& entry : entries_) {
        if (entry.id == shader_id) {
            return &entry.program;
        }
    }
    return nullptr;
}

WgslProgram batched2d_wgsl_program() {
    WgslProgram program;
    program.source = kBatched2dWgsl;
    return program;
}

ShaderRegistry builtin_shader_registry() {
    ShaderRegistry registry;
    registry.add(gpu::kBatched2dShaderId, batched2d_wgsl_program());
    return registry;
}

} // namespace tessera::rendering::webgpu
