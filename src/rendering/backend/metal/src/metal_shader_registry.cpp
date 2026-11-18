// SPDX-License-Identifier: MIT

#include "tessera/rendering/metal/metal_shader_registry.hpp"

#include "tessera/rendering/gpu/shader_ids.hpp"

#include <array>
#include <cstddef>

// The MSL sources are embedded at configure time by the backend's CMakeLists.txt when
// TESSERA_SHADER_CROSS is ON (ADR-049). Without it the table is empty and a Metal pipeline that
// asks for a shader id fails loudly rather than drawing nothing - the same "no silent fallback"
// rule the rest of the RHI follows.
#if defined(TESSERA_METAL_HAS_MSL)

// Each .inc is one raw string literal, so it is bound to a named constant here rather than pasted
// at file scope.
constexpr char kBatched2dVertMsl[] =
#include "batched2d_vert_msl.inc"
    ;
constexpr char kBatched2dFragMsl[] =
#include "batched2d_frag_msl.inc"
    ;
constexpr char kGlassBlurVertMsl[] =
#include "glass_blur_vert_msl.inc"
    ;
constexpr char kGlassBlurFragMsl[] =
#include "glass_blur_frag_msl.inc"
    ;

#endif

namespace tessera::rendering::metal {
namespace {

struct Entry {
    std::string_view id;
    MslProgram program;
};

#ifdef TESSERA_METAL_HAS_MSL

// SPIRV-Cross emits `main0` for a GLSL `main` in both stages, so the entry names are the same for
// every program here. They are recorded per program anyway: a future shader with a different entry
// name must not silently inherit this one.
constexpr std::array<Entry, 2> kEntries{{
    {gpu::kBatched2dShaderId,
     MslProgram{kBatched2dVertMsl, kBatched2dFragMsl, "main0", "main0"}},
    {gpu::kGlassBlurShaderId,
     MslProgram{kGlassBlurVertMsl, kGlassBlurFragMsl, "main0", "main0"}},
}};

#else

constexpr std::array<Entry, 0> kEntries{};

#endif

} // namespace

const MslProgram* find_msl_program(std::string_view shader_id) {
    if (shader_id.empty()) {
        return nullptr;
    }
    for (const Entry& entry : kEntries) {
        if (entry.id == shader_id) {
            return &entry.program;
        }
    }
    return nullptr;
}

std::vector<std::string_view> msl_shader_ids() {
    std::vector<std::string_view> ids;
    ids.reserve(kEntries.size());
    for (const Entry& entry : kEntries) {
        ids.push_back(entry.id);
    }
    return ids;
}

bool has_msl_shaders() {
    return !kEntries.empty();
}

} // namespace tessera::rendering::metal
