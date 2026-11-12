// SPDX-License-Identifier: MIT

#include "tessera/rendering/webgpu/shader_registry.hpp"

namespace {

constexpr char kBatched2dWgsl[] =
#include "batched2d_wgsl.inc"
    ;

} // namespace

namespace tessera::rendering::webgpu {

void ShaderRegistry::add(const std::uint32_t* key, const WgslProgram& program) {
    for (Entry& entry : entries_) {
        if (entry.key == key) {
            entry.program = program;
            return;
        }
    }
    entries_.push_back(Entry{key, program});
}

const WgslProgram* ShaderRegistry::find(const std::uint32_t* key) const {
    if (key == nullptr) {
        return nullptr;
    }
    for (const Entry& entry : entries_) {
        if (entry.key == key) {
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

} // namespace tessera::rendering::webgpu
