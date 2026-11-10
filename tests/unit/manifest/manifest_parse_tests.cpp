// SPDX-License-Identifier: MIT

// Shader manifest shape validation (docs/specs/shader-manifest-v1.md sections 1-3): a valid
// element manifest, a minimal manifest with defaults, a valid view-transition effect, key-order
// independence, and the file loader's recoverable failure.

#include "manifest_test_util.hpp"
#include "tessera/schema/manifest.hpp"

#include <string>
#include <string_view>

namespace {

using manifest_test::Checker;
using manifest_test::expect_error;
using manifest_test::expect_ok;
using tessera::schema::ShaderBindingType;
using tessera::schema::ShaderKind;
using tessera::schema::ShaderStage;
using tessera::schema::ValidationErrorCode;

constexpr std::string_view kElementManifest = R"json({
  "manifestVersion": 1,
  "shaders": [
    {
      "name": "batched2d",
      "stages": ["vertex", "fragment"],
      "entry": { "vertex": "main", "fragment": "main" },
      "sources": {
        "vulkan": ["batched2d.vert", "batched2d.frag"],
        "webgpu": ["batched2d.wgsl"]
      },
      "vertexLayout": [
        { "location": 0, "format": "float2", "semantic": "position" },
        { "location": 1, "format": "float2", "semantic": "uv" },
        { "location": 2, "format": "ubyte4norm", "semantic": "color" },
        { "location": 3, "format": "float2", "semantic": "local" },
        { "location": 4, "format": "float2", "semantic": "half_size" },
        { "location": 5, "format": "float", "semantic": "radius" },
        { "location": 6, "format": "float", "semantic": "mode" }
      ],
      "bindings": [
        { "group": 0, "binding": 0, "type": "uniform_buffer", "name": "push" },
        { "group": 0, "binding": 1, "type": "sampler", "name": "samp" },
        { "group": 0, "binding": 2, "type": "sampled_texture", "name": "tex" }
      ],
      "pushConstants": {
        "size": 16,
        "fields": [
          { "name": "viewport_width", "type": "float", "offset": 0 },
          { "name": "viewport_height", "type": "float", "offset": 4 },
          { "name": "aa_radius", "type": "float", "offset": 8 },
          { "name": "pad", "type": "float", "offset": 12 }
        ]
      },
      "tests": [
        { "name": "solid_quad", "golden": "tests/golden/gpu/shaders/batched2d_solid.png" },
        { "name": "sdf_rounded", "golden": "tests/golden/gpu/shaders/batched2d_sdf.png" }
      ]
    }
  ]
})json";

constexpr std::string_view kMinimalManifest =
    R"json({"manifestVersion":1,"shaders":[{"name":"minimal","stages":["fragment"],"sources":{"vulkan":["minimal.frag"]},"tests":[{"name":"smoke"}]}]})json";

constexpr std::string_view kViewTransitionManifest = R"json({
  "manifestVersion": 1,
  "shaders": [
    {
      "name": "fade",
      "kind": "view-transition-effect",
      "stages": ["vertex", "fragment"],
      "sources": {
        "vulkan": ["fade.vert", "fade.frag"],
        "webgpu": ["fade.wgsl"]
      },
      "vertexLayout": [],
      "bindings": [
        { "group": 0, "binding": 0, "type": "uniform_buffer", "name": "transition" },
        { "group": 0, "binding": 1, "type": "sampler", "name": "samp" },
        { "group": 0, "binding": 2, "type": "sampled_texture", "name": "old" },
        { "group": 0, "binding": 3, "type": "sampled_texture", "name": "new" }
      ],
      "tests": [
        { "name": "smoke" },
        { "name": "golden", "golden": "tests/golden/gpu/shaders/fade.png" }
      ]
    }
  ]
})json";

void test_element_manifest(Checker& checker) {
    const auto result = tessera::schema::parse_manifest(kElementManifest);
    expect_ok(checker, result, "element manifest parses");
    if (!result) {
        return;
    }
    const auto& manifest = *result;
    checker.check(manifest.version == 1, "element manifest version");
    checker.check(manifest.shaders.size() == 1, "element manifest has one shader");
    const auto& shader = manifest.shaders.at(0);
    checker.check(shader.name == "batched2d", "element shader name");
    checker.check(shader.kind == ShaderKind::Element, "element shader kind defaults to element");
    checker.check(shader.stages.size() == 2 && shader.stages[0] == ShaderStage::Vertex &&
                      shader.stages[1] == ShaderStage::Fragment,
                  "element shader stages");
    checker.check(shader.entries.size() == 2 && shader.entries[0].stage == ShaderStage::Vertex &&
                      shader.entries[0].entry == "main" &&
                      shader.entries[1].stage == ShaderStage::Fragment &&
                      shader.entries[1].entry == "main",
                  "element entry points");
    checker.check(shader.vulkan_sources.size() == 2 && shader.vulkan_sources[0] == "batched2d.vert",
                  "element Vulkan sources");
    checker.check(shader.webgpu_sources.size() == 1 && shader.webgpu_sources[0] == "batched2d.wgsl",
                  "element WebGPU sources");
    checker.check(shader.vertex_layout.size() == 7, "element vertex layout size");
    checker.check(shader.vertex_layout[2].location == 2 &&
                      shader.vertex_layout[2].format == "ubyte4norm" &&
                      shader.vertex_layout[2].semantic == "color",
                  "element vertex layout row");
    checker.check(shader.bindings.size() == 3, "element bindings size");
    checker.check(shader.bindings[0].group == 0 && shader.bindings[0].binding == 0 &&
                      shader.bindings[0].type == ShaderBindingType::UniformBuffer &&
                      shader.bindings[0].name == "push",
                  "element uniform binding");
    checker.check(shader.bindings[1].type == ShaderBindingType::Sampler &&
                      shader.bindings[2].type == ShaderBindingType::SampledTexture,
                  "element sampler and texture bindings");
    checker.check(shader.push_constants.has_value(), "element push constants present");
    if (shader.push_constants) {
        checker.check(shader.push_constants->size == 16, "element push constant size");
        checker.check(shader.push_constants->fields.size() == 4, "element push constant fields");
        checker.check(shader.push_constants->fields[1].name == "viewport_height" &&
                          shader.push_constants->fields[1].offset == 4,
                      "element push constant field");
    }
    checker.check(shader.tests.size() == 2, "element tests size");
    checker.check(shader.tests[0].name == "solid_quad" &&
                      shader.tests[0].golden == "tests/golden/gpu/shaders/batched2d_solid.png",
                  "element first test");
    checker.check(!shader.tests[1].golden.empty(), "element second test has a golden");
}

void test_minimal_manifest(Checker& checker) {
    const auto result = tessera::schema::parse_manifest(kMinimalManifest);
    expect_ok(checker, result, "minimal manifest parses");
    if (!result || result->shaders.size() != 1) {
        checker.check(false, "minimal manifest has one shader");
        return;
    }
    const auto& shader = result->shaders[0];
    checker.check(shader.kind == ShaderKind::Element, "minimal kind defaults to element");
    checker.check(shader.entries.size() == 1 && shader.entries[0].entry == "main",
                  "minimal entry point defaults to main");
    checker.check(shader.webgpu_sources.empty(), "minimal WebGPU sources default to empty");
    checker.check(shader.vertex_layout.empty() && shader.bindings.empty() &&
                      !shader.push_constants.has_value(),
                  "minimal optional fields default to empty");
    checker.check(shader.tests.size() == 1 && shader.tests[0].golden.empty(),
                  "minimal smoke test has no golden");
}

void test_view_transition_manifest(Checker& checker) {
    const auto result = tessera::schema::parse_manifest(kViewTransitionManifest);
    expect_ok(checker, result, "view-transition manifest parses");
    if (!result || result->shaders.size() != 1) {
        checker.check(false, "view-transition manifest has one shader");
        return;
    }
    const auto& shader = result->shaders[0];
    checker.check(shader.kind == ShaderKind::ViewTransitionEffect, "view-transition kind");
    checker.check(shader.vertex_layout.empty(), "view-transition has no vertex layout");
    checker.check(!shader.push_constants.has_value(), "view-transition has no push constants");
    checker.check(shader.bindings.size() == 4, "view-transition binding count");
    checker.check(shader.tests.size() == 2 && !shader.tests[1].golden.empty(),
                  "view-transition has a golden");
}

void test_determinism(Checker& checker) {
    const auto first = tessera::schema::parse_manifest(kElementManifest);
    const auto second = tessera::schema::parse_manifest(kElementManifest);
    checker.check(first.has_value() && second.has_value() && *first == *second,
                  "the same bytes produce the same manifest");

    const std::string reordered =
        R"json({"shaders":[{"tests":[{"name":"smoke"}],"sources":{"vulkan":["minimal.frag"]},"stages":["fragment"],"name":"minimal"}],"manifestVersion":1})json";
    const auto reordered_result = tessera::schema::parse_manifest(reordered);
    checker.check(reordered_result.has_value() && reordered_result->shaders.size() == 1 &&
                      reordered_result->shaders[0].name == "minimal",
                  "reordered document parses independently");
    const auto minimal = tessera::schema::parse_manifest(kMinimalManifest);
    checker.check(minimal.has_value() && reordered_result.has_value() &&
                      *minimal == *reordered_result,
                  "key order and whitespace do not change the manifest");
}

void test_loader_missing_file(Checker& checker) {
    const auto result =
        tessera::schema::load_manifest_file("manifest-file-that-does-not-exist-87.json");
    expect_error(checker, result, ValidationErrorCode::FileUnreadable, "$",
                 "missing manifest file");
}

} // namespace

int main() {
    Checker checker;
    test_element_manifest(checker);
    test_minimal_manifest(checker);
    test_view_transition_manifest(checker);
    test_determinism(checker);
    test_loader_missing_file(checker);
    return checker.finish();
}
