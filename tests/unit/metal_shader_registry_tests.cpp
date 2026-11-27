// SPDX-License-Identifier: MIT

// The MSL shader table (ADR-053 decision 2). API-free, so it runs on every host: the table is
// either populated from the cross-compiled MSL or empty, and both states must behave predictably.

#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/metal/metal_shader_registry.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tessera::rendering;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "metal_shader_registry: " << message << "\n";
    }
    return condition;
}

bool check_unknown_id_is_null() {
    return expect(metal::find_msl_program("") == nullptr, "an empty id has no program") &&
           expect(metal::find_msl_program("no_such_shader") == nullptr,
                  "an unknown id has no program");
}

bool check_table_matches_the_build() {
    // The table is populated only when the build embedded MSL (TESSERA_SHADER_CROSS=ON). Both
    // states are valid; what must never happen is a half-populated table, because a pipeline that
    // resolves one id and not another would fail at run time on a real frame.
    const std::vector<std::string_view> ids = metal::msl_shader_ids();
    if (!metal::has_msl_shaders()) {
        return expect(ids.empty(), "an empty table reports no ids") &&
               expect(metal::find_msl_program(gpu::kBatched2dShaderId) == nullptr,
                      "an empty table resolves nothing");
    }
    return expect(!ids.empty(), "a populated table reports its ids") &&
           expect(ids.size() == 3, "the table holds batched2d, glass_blur and the storage probe");
}

bool check_known_ids_resolve() {
    if (!metal::has_msl_shaders()) {
        // Nothing embedded in this configuration; the empty-table case is covered above.
        return true;
    }
    const metal::MslProgram* batched2d = metal::find_msl_program(gpu::kBatched2dShaderId);
    const metal::MslProgram* glass = metal::find_msl_program(gpu::kGlassBlurShaderId);
    if (!expect(batched2d != nullptr, "batched2d resolves") ||
        !expect(glass != nullptr, "glass_blur resolves")) {
        return false;
    }
    // A program with no source would compile an empty library, which is the failure mode this
    // table exists to prevent. Both stages must be present.
    if (!expect(!batched2d->vertex_source.empty(), "batched2d has vertex MSL") ||
        !expect(!batched2d->fragment_source.empty(), "batched2d has fragment MSL") ||
        !expect(!glass->vertex_source.empty(), "glass_blur has vertex MSL") ||
        !expect(!glass->fragment_source.empty(), "glass_blur has fragment MSL")) {
        return false;
    }
    // SPIRV-Cross emits `main0` for a GLSL `main`; the entry names are recorded, not assumed.
    if (!expect(batched2d->vertex_entry == "main0", "batched2d vertex entry is main0") ||
        !expect(batched2d->fragment_entry == "main0", "batched2d fragment entry is main0")) {
        return false;
    }
    // The source must actually be MSL, not an empty string or a stray path: SPIRV-Cross always
    // emits the metal_stdlib include.
    return expect(batched2d->vertex_source.find("metal_stdlib") != std::string_view::npos,
                  "batched2d vertex source includes metal_stdlib") &&
           expect(batched2d->fragment_source.find("metal_stdlib") != std::string_view::npos,
                  "batched2d fragment source includes metal_stdlib") &&
           expect(glass->vertex_source.find("metal_stdlib") != std::string_view::npos,
                  "glass_blur vertex source includes metal_stdlib") &&
           expect(glass->fragment_source.find("metal_stdlib") != std::string_view::npos,
                  "glass_blur fragment source includes metal_stdlib");
}

// True when the MSL declares `<name>` as a buffer argument at exactly `[[buffer(index)]]`.
bool declares_slot(std::string_view source, std::string_view name, std::uint32_t index) {
    const std::string needle =
        "& " + std::string(name) + " [[buffer(" + std::to_string(index) + ")]]";
    return source.find(needle) != std::string_view::npos;
}

// The command list binds the push-constant block and the storage buffer at the slots recorded in
// the registry. SPIRV-Cross numbers them, so this pins the recorded values to the generated text:
// a SPIRV-Cross or glslc change that renumbers a slot fails here, not as a wrong buffer at draw
// time.
bool check_buffer_slots_match_the_msl() {
    if (!metal::has_msl_shaders()) {
        return true;
    }
    const metal::MslProgram* batched2d = metal::find_msl_program(gpu::kBatched2dShaderId);
    const metal::MslProgram* glass = metal::find_msl_program(gpu::kGlassBlurShaderId);
    const metal::MslProgram* probe = metal::find_msl_program(metal::kStorageProbeShaderId);
    if (!expect(batched2d != nullptr && glass != nullptr && probe != nullptr,
                "every program resolves")) {
        return false;
    }
    // The vertex stage keeps its push-constant block at buffer(0); the vertex stream lives at
    // kVertexStreamIndex (30, metal_internal.hpp), well clear of it.
    return expect(declares_slot(batched2d->vertex_source, "pc", 0),
                  "batched2d vertex push constants are buffer(0)") &&
           expect(batched2d->fragment_storage_buffer_index != metal::kNoMslBuffer &&
                      declares_slot(batched2d->fragment_source, "clip_buffer",
                                    batched2d->fragment_storage_buffer_index),
                  "batched2d fragment clip_buffer is at its recorded slot") &&
           expect(declares_slot(batched2d->fragment_source, "pc",
                                batched2d->fragment_push_constant_index),
                  "batched2d fragment push constants are at their recorded slot") &&
           expect(declares_slot(glass->fragment_source, "pc", glass->fragment_push_constant_index),
                  "glass_blur fragment push constants are at their recorded slot") &&
           expect(glass->fragment_storage_buffer_index == metal::kNoMslBuffer,
                  "glass_blur declares no storage buffer") &&
           expect(probe->fragment_storage_buffer_index != metal::kNoMslBuffer &&
                      declares_slot(probe->fragment_source, "records",
                                    probe->fragment_storage_buffer_index),
                  "the probe's records buffer is at its recorded slot");
}

bool check_lcd_id_is_absent() {
    // batched2d_lcd is Vulkan-only by contract (its manifest entry has no WebGPU source and the
    // cross step does not build it), so Metal must not resolve it. A pipeline asking for it has to
    // fail rather than silently draw the non-LCD program.
    return expect(metal::find_msl_program(gpu::kBatched2dLcdShaderId) == nullptr,
                  "batched2d_lcd has no MSL program");
}

} // namespace

int main() {
    if (!check_unknown_id_is_null() || !check_table_matches_the_build() ||
        !check_known_ids_resolve() || !check_buffer_slots_match_the_msl() ||
        !check_lcd_id_is_absent()) {
        return 1;
    }
    return 0;
}
