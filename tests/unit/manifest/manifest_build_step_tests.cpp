// SPDX-License-Identifier: MIT

// The shader build step's manifest contract (docs/specs/shader-manifest-v1.md sections 4, 5 and 9):
// the authored manifest loads through tessera_schema, a broken copy fails with the typed error, and
// the batched2d entry matches Vertex2D and the GLSL interface glslc compiles. Sources are read from
// the source tree at run time, the read-only pattern the layering tests use.

#include "manifest_test_util.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/schema/manifest.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_SHADER_MANIFEST_PATH
#error "TESSERA_SHADER_MANIFEST_PATH must name the repository manifest"
#endif

#ifndef TESSERA_SHADER_GLSL_DIR
#error "TESSERA_SHADER_GLSL_DIR must name the Vulkan GLSL source directory"
#endif

namespace {

using manifest_test::Checker;
using manifest_test::erase_first;
using manifest_test::expect_error;
using manifest_test::expect_ok;
using manifest_test::replace_first;
using tessera::rendering::gpu::kBatched2dShaderId;
using tessera::rendering::gpu::kVertex2DAttributeCount;
using tessera::rendering::gpu::kVertex2DAttributes;
using tessera::schema::Manifest;
using tessera::schema::ShaderBindingType;
using tessera::schema::ShaderEntry;
using tessera::schema::ShaderKind;
using tessera::schema::ShaderStage;
using tessera::schema::ValidationErrorCode;

std::string read_file(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Manifest format token of an RHI vertex format, so the manifest cannot drift from Vertex2D.
std::string_view vertex_format_name(tessera::rendering::rhi::VertexFormat format) {
    using tessera::rendering::rhi::VertexFormat;
    if (format == VertexFormat::Float) {
        return "float";
    }
    if (format == VertexFormat::Float2) {
        return "float2";
    }
    if (format == VertexFormat::Float3) {
        return "float3";
    }
    if (format == VertexFormat::Float4) {
        return "float4";
    }
    return "ubyte4norm";
}

// The GLSL type a manifest format is declared as. A normalized byte color is a vec4 in GLSL.
std::string_view glsl_type_name(std::string_view format) {
    if (format == "float") {
        return "float";
    }
    if (format == "float2") {
        return "vec2";
    }
    if (format == "float3") {
        return "vec3";
    }
    return "vec4";
}

std::string_view semantic_name(std::string_view glsl_name) {
    return glsl_name.starts_with("in_") ? glsl_name.substr(3) : glsl_name;
}

// Lines reached through `#ifdef TESSERA_SHADER_PORTABLE`, the interface the WGSL twin and the
// translators share. Mirrors the drift guard in tests/gpu/shaders/shader_parity_tests.cpp.
std::vector<std::string> portable_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::vector<bool> active{true};
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.starts_with("#ifdef TESSERA_SHADER_PORTABLE")) {
            active.push_back(active.back());
        } else if (line.starts_with("#else") && active.size() > 1) {
            active.back() = !active.back() && active[active.size() - 2];
        } else if (line.starts_with("#endif") && active.size() > 1) {
            active.pop_back();
        } else if (active.back()) {
            lines.push_back(line);
        }
    }
    return lines;
}

struct GlslDecl {
    std::string type;
    std::string name;
};

std::map<std::uint32_t, GlslDecl> glsl_inputs(const std::string& text) {
    const std::regex pattern(R"(layout\(location = (\d+)\) in (\w+) (\w+);)");
    std::map<std::uint32_t, GlslDecl> inputs;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, pattern)) {
            inputs[static_cast<std::uint32_t>(std::stoul(match[1]))] = {match[2], match[3]};
        }
    }
    return inputs;
}

// Group-0 bindings of the portable GLSL: the std140 push-constant block, the texture and sampler.
std::map<std::uint32_t, GlslDecl> glsl_bindings(const std::vector<std::string>& lines) {
    const std::regex layout(R"(layout\(set = 0, binding = (\d+)(?:, std140)?\) uniform (\w+))");
    const std::regex uniform_name(R"(uniform (?:texture2D|sampler) (\w+);)");
    const std::regex block_name(R"(^\}\s+(\w+);)");
    std::map<std::uint32_t, GlslDecl> bindings;
    std::uint32_t block_binding = 0;
    bool in_block = false;
    for (const std::string& line : lines) {
        std::smatch match;
        if (std::regex_search(line, match, layout)) {
            const auto binding = static_cast<std::uint32_t>(std::stoul(match[1]));
            const std::string token = match[2];
            if (token == "PushConstants") {
                block_binding = binding;
                in_block = true;
            } else {
                std::smatch name;
                std::regex_search(line, name, uniform_name);
                bindings[binding] = {token == "texture2D" ? "sampled_texture" : "sampler",
                                     name.empty() ? std::string{} : name[1].str()};
            }
        } else if (in_block && std::regex_search(line, match, block_name)) {
            bindings[block_binding] = {"uniform_buffer", match[1]};
            in_block = false;
        }
    }
    return bindings;
}

std::string shader_source(const ShaderEntry& entry, std::string_view name) {
    for (const std::string& source : entry.vulkan_sources) {
        if (source == name) {
            return read_file(std::string(TESSERA_SHADER_GLSL_DIR) + "/" + source);
        }
    }
    return {};
}

const ShaderEntry* find_batched2d(Checker& checker, const Manifest& manifest) {
    checker.check(manifest.shaders.size() == 1, "manifest has one shader entry");
    if (manifest.shaders.size() != 1) {
        return nullptr;
    }
    const ShaderEntry& entry = manifest.shaders.front();
    checker.check(entry.name == kBatched2dShaderId, "manifest name matches the shader id");
    checker.check(entry.kind == ShaderKind::Element, "batched2d is an element shader");
    checker.check(entry.stages.size() == 2 && entry.stages[0] == ShaderStage::Vertex &&
                      entry.stages[1] == ShaderStage::Fragment,
                  "batched2d declares vertex and fragment stages");
    checker.check(entry.entries.size() == 2 && entry.entries[0].entry == "main" &&
                      entry.entries[1].entry == "main",
                  "batched2d entry points are main in both stages");
    return &entry;
}

void test_vertex_layout(Checker& checker, const ShaderEntry& entry) {
    const std::string vertex_source = shader_source(entry, "batched2d.vert");
    const auto inputs = glsl_inputs(vertex_source);

    checker.check(entry.vertex_layout.size() == kVertex2DAttributeCount,
                  "vertex layout covers every Vertex2D attribute");
    checker.check(inputs.size() == kVertex2DAttributeCount, "GLSL declares every Vertex2D input");
    for (std::size_t index = 0; index < entry.vertex_layout.size(); ++index) {
        const auto& attribute = entry.vertex_layout[index];
        checker.check(attribute.location == kVertex2DAttributes[index].location,
                      "vertex location matches Vertex2D");
        checker.check(attribute.format == vertex_format_name(kVertex2DAttributes[index].format),
                      "vertex format matches Vertex2D");
        const auto input = inputs.find(attribute.location);
        checker.check(input != inputs.end(), "GLSL declares the manifest vertex location");
        if (input == inputs.end()) {
            continue;
        }
        checker.check(input->second.type == glsl_type_name(attribute.format),
                      "GLSL vertex type matches the manifest format");
        checker.check(semantic_name(input->second.name) == attribute.semantic,
                      "vertex semantic matches the GLSL input name");
    }
}

void test_bindings(Checker& checker, const ShaderEntry& entry) {
    auto bindings = glsl_bindings(portable_lines(shader_source(entry, "batched2d.vert")));
    const auto fragment = glsl_bindings(portable_lines(shader_source(entry, "batched2d.frag")));
    bindings.insert(fragment.begin(), fragment.end());

    checker.check(bindings.size() == entry.bindings.size(),
                  "manifest bindings match the portable GLSL bindings");
    for (const auto& binding : entry.bindings) {
        checker.check(binding.group == 0, "manifest binding is in group 0");
        const auto glsl = bindings.find(binding.binding);
        checker.check(glsl != bindings.end(), "GLSL declares the manifest binding index");
        if (glsl == bindings.end()) {
            continue;
        }
        checker.check(tessera::schema::to_string(binding.type) == glsl->second.type,
                      "binding type matches the GLSL declaration");
        checker.check(binding.name == glsl->second.name,
                      "binding name matches the GLSL declaration");
    }
}

void test_push_constants(Checker& checker, const ShaderEntry& entry) {
    using tessera::rendering::gpu::Batched2DPushConstants;
    checker.check(entry.push_constants.has_value(), "batched2d declares push constants");
    if (!entry.push_constants) {
        return;
    }
    const auto& constants = *entry.push_constants;
    checker.check(constants.size == sizeof(Batched2DPushConstants),
                  "push-constant size matches Batched2DPushConstants");
    checker.check(constants.fields.size() == 9, "push-constant fields cover the block");
    if (constants.fields.size() != 9) {
        return;
    }
    checker.check(constants.fields[0].name == "viewport" && constants.fields[0].type == "float2" &&
                      constants.fields[0].offset == 0,
                  "viewport is a float2 at offset 0");
    checker.check(constants.fields[1].name == "aa_radius" && constants.fields[1].type == "float" &&
                      constants.fields[1].offset == 8,
                  "aa_radius is a float at offset 8");
    checker.check(constants.fields[2].name == "clip_count" && constants.fields[2].type == "uint" &&
                      constants.fields[2].offset == 12,
                  "clip_count is a uint at offset 12");
    for (std::size_t index = 3; index < constants.fields.size(); ++index) {
        checker.check(constants.fields[index].type == "float4",
                      "clip fields are float4 rect/radii pairs");
        checker.check(constants.fields[index].offset == 16 + (index - 3) * 16,
                      "clip fields are contiguous");
    }
}

void test_broken_copies(Checker& checker, const std::string& text) {
    // The broken copies stay in memory: this test lane is write-free (manifest_test_util.hpp).
    const std::string missing_stages = erase_first(text, "\"stages\": [\"vertex\", \"fragment\"],");
    checker.check(missing_stages != text, "broken copy removed the stages key");
    expect_error(checker, tessera::schema::parse_manifest(missing_stages),
                 ValidationErrorCode::MissingKey, "$.shaders[0].stages",
                 "missing required key fails the build step");

    const std::string wrong_key =
        replace_first(text, "\"semantic\": \"position\"", "\"semantics\": \"position\"");
    expect_error(checker, tessera::schema::parse_manifest(wrong_key),
                 ValidationErrorCode::UnknownKey, "$.shaders[0].vertexLayout[0].semantics",
                 "unknown key fails the build step");
}

} // namespace

int main() {
    Checker checker;
    const std::string text = read_file(TESSERA_SHADER_MANIFEST_PATH);
    checker.check(!text.empty(), "authored manifest is readable from the repository path");
    const auto loaded = tessera::schema::load_manifest_file(TESSERA_SHADER_MANIFEST_PATH);
    expect_ok(checker, loaded, "authored manifest loads from the repository path");
    const auto manifest = tessera::schema::parse_manifest(text);
    expect_ok(checker, manifest, "authored manifest text parses");
    checker.check(loaded.has_value() && manifest.has_value() && *loaded == *manifest,
                  "file loader and text parser agree on the authored manifest");
    if (manifest) {
        const ShaderEntry* entry = find_batched2d(checker, *manifest);
        if (entry != nullptr) {
            test_vertex_layout(checker, *entry);
            test_bindings(checker, *entry);
            test_push_constants(checker, *entry);
        }
    }
    test_broken_copies(checker, text);
    return checker.finish();
}
