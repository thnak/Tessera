// SPDX-License-Identifier: MIT

// Drift guard between the authored batched2d sources (ADR-011, ADR-049): the GLSL that glslc
// compiles for Vulkan and the WGSL twin embedded for WebGPU must declare the same interface. Runs
// in every build without shader tools; it reads the GLSL as text, selects the
// TESSERA_SHADER_PORTABLE branch (the interface translators and WebGPU share), and compares:
// vertex inputs, stage varyings and their interpolation, the uniform block members and their
// std140/WGSL-uniform size, and the group-0 bindings. Semantics (the function bodies) are covered
// by goldens once a WebGPU pixel lane exists.

#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#ifdef TESSERA_TEST_HAS_VULKAN
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace gpu = tessera::rendering::gpu;
namespace rhi = tessera::rendering::rhi;
namespace wg = tessera::rendering::webgpu;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "shader_parity: " << message << "\n";
        ++g_failures;
    }
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Lines without `//` comments; GLSL lines are filtered through the TESSERA_SHADER_PORTABLE branch.
std::vector<std::string> code_lines(const std::string& text, bool glsl) {
    std::vector<std::string> lines;
    std::vector<bool> active{true};
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = line.substr(0, line.find("//"));
        if (glsl && line.starts_with("#ifdef TESSERA_SHADER_PORTABLE")) {
            active.push_back(active.back());
        } else if (glsl && line.starts_with("#else") && active.size() > 1) {
            active.back() = !active.back() && active[active.size() - 2];
        } else if (glsl && line.starts_with("#endif") && active.size() > 1) {
            active.pop_back();
        } else if (active.back()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string wgsl_type(const std::string& glsl) {
    static const std::map<std::string, std::string> kTypes = {{"float", "f32"},
                                                              {"uint", "u32"},
                                                              {"int", "i32"},
                                                              {"vec2", "vec2<f32>"},
                                                              {"vec3", "vec3<f32>"},
                                                              {"vec4", "vec4<f32>"},
                                                              {"texture2D", "texture_2d<f32>"},
                                                              {"sampler", "sampler"}};
    const auto found = kTypes.find(glsl);
    return found == kTypes.end() ? glsl : found->second;
}

struct Varying {
    std::string type;
    bool flat = false;
    bool operator==(const Varying&) const = default;
};
using Locations = std::map<std::uint32_t, Varying>;
using Members = std::vector<std::pair<std::string, std::string>>; // name, WGSL type

// `layout(location = N) [flat] <direction> <type> <name>;`
Locations glsl_locations(const std::vector<std::string>& lines, const std::string& direction) {
    const std::regex pattern(R"(layout\(location = (\d+)\) (flat )?)" + direction +
                             R"( (\w+) \w+;)");
    Locations result;
    std::smatch match;
    for (const std::string& line : lines) {
        if (std::regex_search(line, match, pattern)) {
            result[static_cast<std::uint32_t>(std::stoul(match[1]))] =
                Varying{wgsl_type(match[3]), match[2].matched};
        }
    }
    return result;
}

// Members of a GLSL `struct Name {` or `uniform Name {` block, translated to WGSL types.
Members glsl_block(const std::vector<std::string>& lines, const std::string& opener) {
    const std::regex member(R"(^\s*(\w+) (\w+)(?:\[(\d+)\])?;)");
    Members members;
    bool inside = false;
    std::smatch match;
    for (const std::string& line : lines) {
        if (!inside) {
            inside = line.find(opener) != std::string::npos;
            continue;
        }
        if (line.find('}') != std::string::npos) {
            break;
        }
        if (std::regex_search(line, match, member)) {
            std::string type = wgsl_type(match[1]);
            if (match[3].matched) {
                type = "array<" + type + ", " + match[3].str() + ">";
            }
            members.emplace_back(match[2].str(), type);
        }
    }
    return members;
}

// Members of a WGSL `struct Name {`, with their attribute prefix (`@location(0) ...`).
std::vector<std::pair<std::string, std::pair<std::string, std::string>>>
wgsl_struct(const std::vector<std::string>& lines, const std::string& name) {
    const std::regex member(R"(^\s*((?:@\w+(?:\([^)]*\))?\s+)*)(\w+)\s*:\s*(.+?),\s*$)");
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> members;
    bool inside = false;
    std::smatch match;
    for (const std::string& line : lines) {
        if (!inside) {
            inside = line.find("struct " + name + " {") != std::string::npos;
            continue;
        }
        if (line.find('}') != std::string::npos) {
            break;
        }
        if (std::regex_search(line, match, member)) {
            members.push_back({match[1].str(), {match[2].str(), match[3].str()}});
        }
    }
    return members;
}

Locations wgsl_locations(const std::vector<std::string>& lines, const std::string& name) {
    const std::regex location(R"(@location\((\d+)\))");
    Locations result;
    std::smatch match;
    for (const auto& [attributes, member] : wgsl_struct(lines, name)) {
        if (std::regex_search(attributes, match, location)) {
            result[static_cast<std::uint32_t>(std::stoul(match[1]))] =
                Varying{member.second, attributes.find("@interpolate(flat)") != std::string::npos};
        }
    }
    return result;
}

Members wgsl_members(const std::vector<std::string>& lines, const std::string& name) {
    Members members;
    for (const auto& [attributes, member] : wgsl_struct(lines, name)) {
        members.push_back(member);
    }
    return members;
}

// Size/alignment under WGSL uniform rules, which equal std140 for the types used here.
std::pair<std::uint32_t, std::uint32_t> layout_of(const std::string& type,
                                                  const std::map<std::string, Members>& structs);

std::pair<std::uint32_t, std::uint32_t> struct_layout(const Members& members,
                                                      const std::map<std::string, Members>& s) {
    std::uint32_t offset = 0;
    std::uint32_t align = 16; // uniform-address-space structs round to 16 (std140 too)
    for (const auto& [name, type] : members) {
        const auto [size, member_align] = layout_of(type, s);
        offset = (offset + member_align - 1) / member_align * member_align + size;
        align = std::max(align, member_align);
    }
    return {(offset + align - 1) / align * align, align};
}

std::pair<std::uint32_t, std::uint32_t> layout_of(const std::string& type,
                                                  const std::map<std::string, Members>& structs) {
    static const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> kScalars = {
        {"f32", {4, 4}},       {"u32", {4, 4}},         {"i32", {4, 4}},
        {"vec2<f32>", {8, 8}}, {"vec3<f32>", {12, 16}}, {"vec4<f32>", {16, 16}}};
    static const std::regex array(R"(array<(\w+), (\d+)>)");
    std::smatch match;
    if (const auto scalar = kScalars.find(type); scalar != kScalars.end()) {
        return scalar->second;
    }
    if (std::regex_match(type, match, array)) {
        const auto [size, align] = layout_of(match[1], structs);
        const std::uint32_t stride = (size + 15) / 16 * 16; // uniform arrays stride by 16
        return {stride * static_cast<std::uint32_t>(std::stoul(match[2])), std::max(align, 16u)};
    }
    if (const auto found = structs.find(type); found != structs.end()) {
        return struct_layout(found->second, structs);
    }
    expect(false, "unknown type in uniform layout: " + type);
    return {0, 4};
}

// `@group(0) @binding(N) var[<uniform>] name: type;` -> binding -> type ("uniform" for buffers).
std::map<std::uint32_t, std::string> wgsl_bindings(const std::vector<std::string>& lines) {
    const std::regex pattern(R"(@group\(0\) @binding\((\d+)\) var(<uniform>)? \w+: ([^;]+);)");
    std::map<std::uint32_t, std::string> result;
    std::smatch match;
    for (const std::string& line : lines) {
        if (std::regex_search(line, match, pattern)) {
            result[static_cast<std::uint32_t>(std::stoul(match[1]))] =
                match[2].matched ? "uniform" : match[3].str();
        }
    }
    return result;
}

// `layout(set = 0, binding = N[, std140]) uniform <type|Block> name;`
std::map<std::uint32_t, std::string> glsl_bindings(const std::vector<std::string>& lines) {
    const std::regex pattern(R"(layout\(set = 0, binding = (\d+)(?:, std140)?\) uniform (\w+))");
    std::map<std::uint32_t, std::string> result;
    std::smatch match;
    for (const std::string& line : lines) {
        if (std::regex_search(line, match, pattern)) {
            const std::string type = wgsl_type(match[2]);
            result[static_cast<std::uint32_t>(std::stoul(match[1]))] =
                type == "PushConstants" ? "uniform" : type;
        }
    }
    return result;
}

void test_batched2d_parity() {
    const std::string dir = TESSERA_VULKAN_SHADER_DIR;
    const std::string vert_text = read_file(dir + "/batched2d.vert");
    const std::string frag_text = read_file(dir + "/batched2d.frag");
    expect(!vert_text.empty() && !frag_text.empty(), "GLSL sources readable from " + dir);
    const auto vert = code_lines(vert_text, true);
    const auto frag = code_lines(frag_text, true);
    const auto wgsl = code_lines(std::string(wg::batched2d_wgsl_program().source), false);

    const Locations inputs = glsl_locations(vert, "in");
    expect(inputs == wgsl_locations(wgsl, "VertexInput"), "vertex inputs differ");
    expect(inputs.size() == gpu::kVertex2DAttributeCount, "GLSL inputs cover Vertex2D");
    for (const rhi::VertexAttribute& attribute : gpu::kVertex2DAttributes) {
        expect(inputs.contains(attribute.location), "Vertex2D location has a shader input");
    }

    const Locations varyings = glsl_locations(vert, "out");
    expect(!varyings.empty() && varyings == glsl_locations(frag, "in"),
           "GLSL vertex outputs differ from fragment inputs");
    expect(varyings == wgsl_locations(wgsl, "VertexOutput"),
           "varyings (type or flat interpolation) differ from WGSL VertexOutput");

    const Members clip = glsl_block(frag, "struct ClipUniform {");
    expect(!clip.empty() && clip == glsl_block(vert, "struct ClipUniform {") &&
               clip == wgsl_members(wgsl, "ClipUniform"),
           "ClipUniform members differ");
    const Members block = glsl_block(frag, "uniform PushConstants {");
    expect(!block.empty() && block == glsl_block(vert, "uniform PushConstants {") &&
               block == wgsl_members(wgsl, "Params"),
           "uniform block members differ between GLSL PushConstants and WGSL Params");
    const std::map<std::string, Members> structs = {{"ClipUniform", clip}};
    expect(struct_layout(block, structs).first == sizeof(gpu::Batched2DPushConstants),
           "uniform block size differs from Batched2DPushConstants");

    const auto bindings = wgsl_bindings(wgsl);
    expect(bindings == glsl_bindings(frag), "portable GLSL bindings differ from WGSL bindings");
    expect(bindings.size() == 3 && bindings.at(wg::kBindingPushConstants) == "uniform" &&
               bindings.at(wg::kBindingTexture) == "texture_2d<f32>" &&
               bindings.at(wg::kBindingSampler) == "sampler",
           "WGSL bindings match the WebGPU pipeline layout constants");
    const auto vert_bindings = glsl_bindings(vert);
    expect(vert_bindings.size() == 1 && vert_bindings.contains(wg::kBindingPushConstants),
           "portable vertex stage binds only the uniform block");
}

void test_shader_id() {
    expect(gpu::kBatched2dShaderId == "batched2d", "batched2d id is the manifest name");
    expect(rhi::PipelineDesc{}.shader_id.empty(), "shader_id defaults to empty (SPIR-V words)");
#ifdef TESSERA_TEST_HAS_VULKAN
    expect(tessera::rendering::vulkan::batched2d_pipeline_desc().shader_id ==
               gpu::kBatched2dShaderId,
           "batched2d pipeline descriptor carries its shader id");
#endif
}

} // namespace

int main() {
    test_batched2d_parity();
    test_shader_id();
    if (g_failures != 0) {
        std::cerr << "shader_parity: " << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
