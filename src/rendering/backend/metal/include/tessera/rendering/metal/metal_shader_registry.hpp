// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::rendering::metal {

// The MSL shader table (ADR-053 decision 2, docs/guides/writing-a-backend.md section 5).
//
// Metal consumes MSL, not SPIR-V, so a pipeline created with `PipelineDesc::shader_id` resolves the
// id against this table instead of reading `PipelineDesc::vertex_spirv`. The MSL is generated at
// build time by ADR-049's `tessera_cross_compile_shader` path (GLSL -> SPIR-V -> MSL via
// SPIRV-Cross) and embedded as a string, so production compiles no shader at run time.
//
// This header is API-free: no `<Metal/Metal.h>`, no Objective-C. The device half hands the source
// to `newLibraryWithSource:` (or a precompiled metallib) and the registry itself is unit-tested on
// every host, exactly like the mapping tables.
//
// A program is one MSL source string plus the entry-point names. SPIRV-Cross emits `main0` for a
// GLSL `main`, so the entry names are recorded rather than assumed.

struct MslProgram {
    // The MSL sources, one per stage. Empty means "this id has no MSL program", which a pipeline
    // must reject rather than compile an empty library.
    std::string_view vertex_source;
    std::string_view fragment_source;
    // Entry-point names in the sources above. SPIRV-Cross emits `main0` for a GLSL `main`, so the
    // names are recorded rather than assumed.
    std::string_view vertex_entry = "main0";
    std::string_view fragment_entry = "main0";
};

// Resolves a manifest shader id to its MSL program. Returns nullptr for an unknown id, so a caller
// can report "no MSL for this shader" instead of compiling nothing.
[[nodiscard]] const MslProgram* find_msl_program(std::string_view shader_id);

// The ids this build has MSL for. Empty when the build was configured without
// TESSERA_SHADER_CROSS, which is the default: a Metal build must turn it on (see the backend
// README), and a pipeline that asks for an id with no MSL fails loudly instead of drawing nothing.
[[nodiscard]] std::vector<std::string_view> msl_shader_ids();

// True when this build embedded any MSL at all.
[[nodiscard]] bool has_msl_shaders();

} // namespace tessera::rendering::metal
