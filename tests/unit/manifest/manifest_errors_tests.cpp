// SPDX-License-Identifier: MIT

// Shader manifest error-path stability (docs/specs/shader-manifest-v1.md section 9): the error
// enum has a stable printable token for every value, field paths have one spelling per location,
// repeated validation of the same bytes produces an identical error, and the file loader reports
// unreadable/oversized files without throwing.

#include "manifest_test_util.hpp"
#include "tessera/schema/manifest.hpp"

#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace {

using manifest_test::Checker;
using manifest_test::expect_error;
using manifest_test::replace_first;
using tessera::schema::ShaderBindingType;
using tessera::schema::ShaderKind;
using tessera::schema::ShaderStage;
using tessera::schema::ValidationError;
using tessera::schema::ValidationErrorCode;

constexpr std::string_view kElement =
    R"json({"manifestVersion":1,"shaders":[{"name":"s","stages":["vertex","fragment"],"sources":{"vulkan":["s.vert","s.frag"]},"vertexLayout":[{"location":0,"format":"float2","semantic":"position"},{"location":0,"format":"float2","semantic":"color"}],"tests":[{"name":"smoke"}]}]})json";

constexpr std::string_view kMinimalManifest =
    R"json({"manifestVersion":1,"shaders":[{"name":"minimal","stages":["fragment"],"sources":{"vulkan":["minimal.frag"]},"tests":[{"name":"smoke"}]}]})json";

void test_error_tokens(Checker& checker) {
    using enum ValidationErrorCode;
    constexpr std::pair<ValidationErrorCode, std::string_view> kTokens[] = {
        {InvalidJson, "invalid_json"},
        {JsonTooDeep, "json_too_deep"},
        {FileTooLarge, "file_too_large"},
        {FileUnreadable, "file_unreadable"},
        {RootNotObject, "root_not_object"},
        {MissingKey, "missing_key"},
        {UnknownKey, "unknown_key"},
        {WrongType, "wrong_type"},
        {OutOfRange, "out_of_range"},
        {InvalidValue, "invalid_value"},
        {UnsupportedVersion, "unsupported_version"},
        {UnknownStage, "unknown_stage"},
        {DuplicateStage, "duplicate_stage"},
        {ComputeStageUnsupported, "compute_stage_unsupported"},
        {EntryStageNotDeclared, "entry_stage_not_declared"},
        {DuplicateShaderName, "duplicate_shader_name"},
        {DuplicateVertexLocation, "duplicate_vertex_location"},
        {DuplicateVertexSemantic, "duplicate_vertex_semantic"},
        {VertexLayoutRequired, "vertex_layout_required"},
        {VertexLayoutNotAllowed, "vertex_layout_not_allowed"},
        {DuplicateBinding, "duplicate_binding"},
        {InvalidPushConstants, "invalid_push_constants"},
        {DuplicateTestName, "duplicate_test_name"},
        {ViewTransitionInterface, "view_transition_interface"},
    };
    for (const auto& [code, token] : kTokens) {
        checker.check(tessera::schema::to_string(code) == token,
                      std::string("stable token for ") + std::string(token));
    }
    checker.check(tessera::schema::to_string(ShaderKind::Element) == "element",
                  "kind token element");
    checker.check(tessera::schema::to_string(ShaderKind::ViewTransitionEffect) ==
                      "view-transition-effect",
                  "kind token view-transition-effect");
    checker.check(tessera::schema::to_string(ShaderStage::Vertex) == "vertex",
                  "stage token vertex");
    checker.check(tessera::schema::to_string(ShaderStage::Fragment) == "fragment",
                  "stage token fragment");
    checker.check(tessera::schema::to_string(ShaderStage::Compute) == "compute",
                  "reserved stage token compute");
    checker.check(tessera::schema::to_string(ShaderBindingType::UniformBuffer) == "uniform_buffer",
                  "binding token uniform_buffer");
    checker.check(tessera::schema::to_string(ShaderBindingType::StorageBuffer) == "storage_buffer",
                  "binding token storage_buffer");
    checker.check(tessera::schema::to_string(ShaderBindingType::Sampler) == "sampler",
                  "binding token sampler");
    checker.check(tessera::schema::to_string(ShaderBindingType::SampledTexture) ==
                      "sampled_texture",
                  "binding token sampled_texture");
    checker.check(tessera::schema::to_string(ShaderBindingType::StorageTexture) ==
                      "storage_texture",
                  "binding token storage_texture");
}

void test_path_spelling(Checker& checker) {
    const auto missing_name = tessera::schema::parse_manifest(
        replace_first(kMinimalManifest, R"("name":"minimal",)", ""));
    checker.check(!missing_name && missing_name.error().path == "$.shaders[0].name",
                  "shader field path");
    checker.check(!missing_name && !missing_name.error().detail.empty(),
                  "errors carry a non-empty static detail");

    const auto bad_stage = tessera::schema::parse_manifest(replace_first(
        kElement, R"("stages":["vertex","fragment"])", R"("stages":["vertex","compute"])"));
    checker.check(!bad_stage && bad_stage.error().path == "$.shaders[0].stages[1]",
                  "array element path");

    const auto bad_source = tessera::schema::parse_manifest(replace_first(
        kMinimalManifest, R"("vulkan":["minimal.frag"])", R"("vulkan":["a/b.frag"])"));
    checker.check(!bad_source && bad_source.error().path == "$.shaders[0].sources.vulkan[0]",
                  "nested array path");

    const auto duplicate_location = tessera::schema::parse_manifest(kElement);
    checker.check(!duplicate_location &&
                      duplicate_location.error().path == "$.shaders[0].vertexLayout[1].location",
                  "vertex layout path");

    const auto duplicate_shader = tessera::schema::parse_manifest(
        R"json({"manifestVersion":1,"shaders":[{"name":"d","stages":["fragment"],"sources":{"vulkan":["a.frag"]},"tests":[{"name":"t"}]},{"name":"d","stages":["fragment"],"sources":{"vulkan":["b.frag"]},"tests":[{"name":"t"}]}]})json");
    checker.check(!duplicate_shader && duplicate_shader.error().path == "$.shaders[1].name",
                  "second shader path");
}

void test_error_determinism(Checker& checker) {
    const auto first = tessera::schema::parse_manifest(kElement);
    const auto second = tessera::schema::parse_manifest(kElement);
    checker.check(!first && !second && first.error() == second.error(),
                  "the same bytes produce the same error");

    const ValidationError duplicate{ValidationErrorCode::DuplicateStage, "$.shaders[0].stages[1]",
                                    "stage is declared twice", 0};
    const ValidationError same{ValidationErrorCode::DuplicateStage, "$.shaders[0].stages[1]",
                               "stage is declared twice", 0};
    checker.check(duplicate == same, "ValidationError equality is deterministic");
}

void test_loader_round_trip(Checker& checker) {
    constexpr std::string_view kPath = "manifest_errors_round_trip_87.json";
    {
        std::ofstream file(std::string(kPath), std::ios::binary | std::ios::trunc);
        file << std::string(kMinimalManifest);
    }
    const auto loaded = tessera::schema::load_manifest_file(kPath);
    checker.check(loaded.has_value() && loaded->shaders.size() == 1 &&
                      loaded->shaders[0].name == "minimal",
                  "loader reads and validates a manifest");
    const auto direct = tessera::schema::parse_manifest(kMinimalManifest);
    checker.check(loaded.has_value() && direct.has_value() && *loaded == *direct,
                  "loader result matches the text parser");
    static_cast<void>(std::remove(std::string(kPath).c_str()));

    constexpr std::string_view kOversizedPath = "manifest_errors_oversized_87.json";
    {
        std::ofstream file(std::string(kOversizedPath), std::ios::binary | std::ios::trunc);
        const std::string oversized(tessera::schema::kMaxManifestBytes + 1, ' ');
        file << oversized;
    }
    expect_error(checker, tessera::schema::load_manifest_file(kOversizedPath),
                 ValidationErrorCode::FileTooLarge, "$", "loader rejects an oversized file");
    static_cast<void>(std::remove(std::string(kOversizedPath).c_str()));
}

void test_syntax_error_offsets(Checker& checker) {
    const auto truncated = tessera::schema::parse_manifest(R"({"manifestVersion":1,)");
    checker.check(!truncated && truncated.error().code == ValidationErrorCode::InvalidJson &&
                      truncated.error().offset > 0,
                  "syntax errors carry a byte offset");
    const std::string deep =
        R"({"manifestVersion":1,"shaders":)" + std::string(40, '[') + std::string(40, ']') + "}";
    const auto nested = tessera::schema::parse_manifest(deep);
    checker.check(!nested && nested.error().code == ValidationErrorCode::JsonTooDeep &&
                      nested.error().offset > 0,
                  "depth errors carry a byte offset");
}

} // namespace

int main() {
    Checker checker;
    test_error_tokens(checker);
    test_path_spelling(checker);
    test_error_determinism(checker);
    test_loader_round_trip(checker);
    test_syntax_error_offsets(checker);
    return checker.finish();
}
