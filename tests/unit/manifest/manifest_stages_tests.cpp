// SPDX-License-Identifier: MIT

// Shader manifest section 4 compatibility rules and the section 3.1 view-transition interface:
// compute/unknown/duplicate stages, entry-point coverage, vertex-layout presence and uniqueness,
// binding/layout duplicates, push-constant layout, test uniqueness, and the fixed effect interface.

#include "manifest_test_util.hpp"
#include "tessera/schema/manifest.hpp"

#include <string>
#include <string_view>

namespace {

using manifest_test::Checker;
using manifest_test::expect_error;
using manifest_test::expect_ok;
using manifest_test::replace_first;
using tessera::schema::ValidationErrorCode;

constexpr std::string_view kElement =
    R"json({"manifestVersion":1,"shaders":[{"name":"s","stages":["vertex","fragment"],"sources":{"vulkan":["s.vert","s.frag"]},"vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],"bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"u"}],"pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]},"tests":[{"name":"smoke"},{"name":"golden","golden":"tests/golden/gpu/shaders/s.png"}]}]})json";

constexpr std::string_view kViewTransition =
    R"json({"manifestVersion":1,"shaders":[{"name":"fade","kind":"view-transition-effect","stages":["vertex","fragment"],"sources":{"vulkan":["fade.vert","fade.frag"],"webgpu":["fade.wgsl"]},"vertexLayout":[],"bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"transition"},{"group":0,"binding":1,"type":"sampler","name":"samp"},{"group":0,"binding":2,"type":"sampled_texture","name":"old"},{"group":0,"binding":3,"type":"sampled_texture","name":"new"}],"tests":[{"name":"smoke"},{"name":"golden","golden":"tests/golden/gpu/shaders/fade.png"}]}]})json";

void test_stage_rules(Checker& checker) {
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("stages":["vertex","fragment"])",
                                                      R"("stages":["vertex","vertex"])")),
        ValidationErrorCode::DuplicateStage, "$.shaders[0].stages[1]", "duplicate vertex stage");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("stages":["vertex","fragment"])",
                                                      R"("stages":["vertex","compute"])")),
        ValidationErrorCode::ComputeStageUnsupported, "$.shaders[0].stages[1]",
        "compute stage is rejected for an element");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("stages":["vertex","fragment"])",
                                                      R"("stages":["vertex","geometry"])")),
        ValidationErrorCode::UnknownStage, "$.shaders[0].stages[1]",
        "geometry stage is rejected for an element");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kElement, R"("stages":["vertex","fragment"])",
                                   R"("stages":["vertex"],"entry":{"fragment":"main"})")),
                 ValidationErrorCode::EntryStageNotDeclared, "$.shaders[0].entry.fragment",
                 "entry point for an undeclared stage");
    const std::string vertex_without_layout = replace_first(
        replace_first(kElement, R"("stages":["vertex","fragment"])", R"("stages":["vertex"])"),
        R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],)", "");
    expect_error(checker, tessera::schema::parse_manifest(vertex_without_layout),
                 ValidationErrorCode::VertexLayoutRequired, "$.shaders[0].vertexLayout",
                 "vertex stage requires a layout key");
}

void test_vertex_layout_rules(Checker& checker) {
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kElement, R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],)",
            "")),
        ValidationErrorCode::VertexLayoutRequired, "$.shaders[0].vertexLayout",
        "missing vertex layout");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kElement, R"("stages":["vertex","fragment"])", R"("stages":["fragment"])")),
                 ValidationErrorCode::VertexLayoutNotAllowed, "$.shaders[0].vertexLayout",
                 "layout without a vertex stage");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kElement, R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"}])",
            R"("vertexLayout":[{"location":0,"format":"float2","semantic":"a"},{"location":0,"format":"float2","semantic":"b"}])")),
        ValidationErrorCode::DuplicateVertexLocation, "$.shaders[0].vertexLayout[1].location",
        "duplicate vertex location");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kElement, R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"}])",
            R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"},{"location":1,"format":"float2","semantic":"position"}])")),
        ValidationErrorCode::DuplicateVertexSemantic, "$.shaders[0].vertexLayout[1].semantic",
        "duplicate vertex semantic");
}

void test_binding_and_test_rules(Checker& checker) {
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kElement, R"("bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"u"}])",
            R"("bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"u"},{"group":0,"binding":0,"type":"sampler","name":"s"}])")),
        ValidationErrorCode::DuplicateBinding, "$.shaders[0].bindings[1].binding",
        "duplicate group/binding pair");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kElement, R"({"name":"golden","golden":"tests/golden/gpu/shaders/s.png"})",
                     R"({"name":"smoke","golden":"tests/golden/gpu/shaders/s.png"})")),
                 ValidationErrorCode::DuplicateTestName, "$.shaders[0].tests[1].name",
                 "duplicate test name");
}

void test_push_constant_rules(Checker& checker) {
    const std::string no_fields = replace_first(
        kElement,
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]})",
        R"("pushConstants":{"size":18,"fields":[{"name":"a","type":"float","offset":0}]})");
    expect_error(checker, tessera::schema::parse_manifest(no_fields),
                 ValidationErrorCode::InvalidPushConstants, "$.shaders[0].pushConstants.size",
                 "push-constant size alignment");
    const std::string misaligned = replace_first(
        kElement,
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]})",
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":2}]})");
    expect_error(checker, tessera::schema::parse_manifest(misaligned),
                 ValidationErrorCode::InvalidPushConstants,
                 "$.shaders[0].pushConstants.fields[0].offset", "push-constant offset alignment");
    const std::string too_large = replace_first(
        kElement,
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]})",
        R"("pushConstants":{"size":8,"fields":[{"name":"a","type":"float2","offset":4}]})");
    expect_error(checker, tessera::schema::parse_manifest(too_large),
                 ValidationErrorCode::InvalidPushConstants,
                 "$.shaders[0].pushConstants.fields[0].offset",
                 "push-constant field exceeds block");
    const std::string overlapping = replace_first(
        kElement,
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]})",
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0},{"name":"b","type":"float","offset":0}]})");
    expect_error(checker, tessera::schema::parse_manifest(overlapping),
                 ValidationErrorCode::InvalidPushConstants,
                 "$.shaders[0].pushConstants.fields[1].offset", "push-constant overlap");
    const std::string duplicate_name = replace_first(
        kElement,
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]})",
        R"("pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0},{"name":"a","type":"float","offset":4}]})");
    expect_error(checker, tessera::schema::parse_manifest(duplicate_name),
                 ValidationErrorCode::InvalidPushConstants,
                 "$.shaders[0].pushConstants.fields[1].name", "push-constant duplicate name");
}

void test_shader_names_and_kind(Checker& checker) {
    constexpr std::string_view kDuplicateNames =
        R"json({"manifestVersion":1,"shaders":[{"name":"dup","stages":["fragment"],"sources":{"vulkan":["a.frag"]},"tests":[{"name":"smoke"}]},{"name":"dup","stages":["fragment"],"sources":{"vulkan":["b.frag"]},"tests":[{"name":"smoke"}]}]})json";
    expect_error(checker, tessera::schema::parse_manifest(kDuplicateNames),
                 ValidationErrorCode::DuplicateShaderName, "$.shaders[1].name",
                 "duplicate shader name");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kViewTransition, R"("kind":"view-transition-effect",)", R"("kind":"vertex-ish",)")),
        ValidationErrorCode::InvalidValue, "$.shaders[0].kind", "unknown shader kind");
}

void test_view_transition_rules(Checker& checker) {
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(kViewTransition,
                                                               R"("stages":["vertex","fragment"])",
                                                               R"("stages":["vertex"])")),
                 ValidationErrorCode::ViewTransitionInterface, "$.shaders[0].stages",
                 "effect stages must be vertex+fragment");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kViewTransition, R"("vertexLayout":[],)",
            R"("vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],)")),
        ValidationErrorCode::VertexLayoutNotAllowed, "$.shaders[0].vertexLayout",
        "effect must not declare a vertex layout");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kViewTransition, R"("vertexLayout":[],)",
            R"("vertexLayout":[],"pushConstants":{"size":16,"fields":[{"name":"a","type":"float","offset":0}]},)")),
        ValidationErrorCode::ViewTransitionInterface, "$.shaders[0].pushConstants",
        "effect must not declare push constants");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kViewTransition, R"(,"webgpu":["fade.wgsl"])", "")),
                 ValidationErrorCode::ViewTransitionInterface, "$.shaders[0].sources.webgpu",
                 "effect requires WebGPU sources");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kViewTransition, R"({"group":0,"binding":1,"type":"sampler","name":"samp"})",
                     R"({"group":0,"binding":1,"type":"sampler","name":"samp2"})")),
                 ValidationErrorCode::ViewTransitionInterface, "$.shaders[0].bindings",
                 "effect binding interface is fixed");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kViewTransition, R"({"name":"golden","golden":"tests/golden/gpu/shaders/fade.png"})",
            R"({"name":"golden"})")),
        ValidationErrorCode::ViewTransitionInterface, "$.shaders[0].tests",
        "effect requires a golden test");
}

void test_shared_element_allows_push_constants(Checker& checker) {
    const std::string shared =
        replace_first(kElement, R"("sources":{"vulkan":["s.vert","s.frag"]})",
                      R"("sources":{"vulkan":["s.vert","s.frag"],"webgpu":["s.wgsl"]})");
    expect_ok(checker, tessera::schema::parse_manifest(shared),
              "shared element may declare Vulkan push constants with a WebGPU uniform fallback");
}

} // namespace

int main() {
    Checker checker;
    test_stage_rules(checker);
    test_vertex_layout_rules(checker);
    test_binding_and_test_rules(checker);
    test_push_constant_rules(checker);
    test_shader_names_and_kind(checker);
    test_view_transition_rules(checker);
    test_shared_element_allows_push_constants(checker);
    return checker.finish();
}
