// SPDX-License-Identifier: MIT

// Shader manifest limits and strict JSON rules (docs/specs/shader-manifest-v1.md section 9):
// version mismatch, scalar bounds, array bounds, oversized input, nesting depth, duplicate keys,
// malformed escapes/UTF-8, and deterministic errors.

#include "manifest_test_util.hpp"
#include "tessera/schema/manifest.hpp"

#include <string>
#include <string_view>

namespace {

using manifest_test::Checker;
using manifest_test::expect_error;
using manifest_test::replace_first;
using tessera::schema::ValidationErrorCode;

constexpr std::string_view kMinimalManifest =
    R"json({"manifestVersion":1,"shaders":[{"name":"minimal","stages":["fragment"],"sources":{"vulkan":["minimal.frag"]},"tests":[{"name":"smoke"}]}]})json";

std::string with_stages(std::string_view stages) {
    return replace_first(kMinimalManifest, R"("stages":["fragment"])", stages);
}

void test_version(Checker& checker) {
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kMinimalManifest, R"("manifestVersion":1)", R"("manifestVersion":2)")),
                 ValidationErrorCode::UnsupportedVersion, "$.manifestVersion",
                 "manifest version 2");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kMinimalManifest, R"("manifestVersion":1)", R"("manifestVersion":0)")),
                 ValidationErrorCode::UnsupportedVersion, "$.manifestVersion",
                 "manifest version 0");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kMinimalManifest, R"("manifestVersion":1)", R"("manifestVersion":1.0)")),
                 ValidationErrorCode::WrongType, "$.manifestVersion", "fractional version");
}

void test_bounds(Checker& checker) {
    const std::string long_name(129, 'a');
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kMinimalManifest, R"("name":"minimal")", R"("name":")" + long_name + R"(")")),
                 ValidationErrorCode::OutOfRange, "$.shaders[0].name", "99 too many name bytes");
    expect_error(checker, tessera::schema::parse_manifest(with_stages(R"("stages":[])")),
                 ValidationErrorCode::InvalidValue, "$.shaders[0].stages", "empty stage list");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     with_stages(R"("stages":["vertex","fragment","vertex","fragment"])")),
                 ValidationErrorCode::OutOfRange, "$.shaders[0].stages", "four stages");
    expect_error(checker, tessera::schema::parse_manifest(with_stages(R"("stages":["geometry"])")),
                 ValidationErrorCode::UnknownStage, "$.shaders[0].stages[0]",
                 "geometry stage is unknown");
    expect_error(checker, tessera::schema::parse_manifest(with_stages(R"("stages":["compute"])")),
                 ValidationErrorCode::ComputeStageUnsupported, "$.shaders[0].stages[0]",
                 "compute stage is rejected");
    expect_error(checker,
                 tessera::schema::parse_manifest(with_stages(R"("stages":["vertex","compute"])")),
                 ValidationErrorCode::ComputeStageUnsupported, "$.shaders[0].stages[1]",
                 "compute after vertex is rejected");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(kMinimalManifest,
                                                               R"("vulkan":["minimal.frag"])",
                                                               R"("vulkan":["sub/minimal.frag"])")),
                 ValidationErrorCode::InvalidValue, "$.shaders[0].sources.vulkan[0]",
                 "source path separators are rejected");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kMinimalManifest, R"({"name":"smoke"})",
                                   R"({"name":"smoke","golden":"/abs/golden.png"})")),
                 ValidationErrorCode::InvalidValue, "$.shaders[0].tests[0].golden",
                 "absolute golden paths are rejected");
}

void test_field_bounds(Checker& checker) {
    constexpr std::string_view kElement =
        R"json({"manifestVersion":1,"shaders":[{"name":"s","stages":["vertex"],"sources":{"vulkan":["s.vert"]},"vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],"bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"u"}],"pushConstants":{"size":16,"fields":[{"name":"f","type":"float","offset":0}]},"tests":[{"name":"smoke"}]}]})json";
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kElement, R"("location":0)", R"("location":32)")),
                 ValidationErrorCode::OutOfRange, "$.shaders[0].vertexLayout[0].location",
                 "vertex location bound");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("group":0)", R"("group":4)")),
        ValidationErrorCode::OutOfRange, "$.shaders[0].bindings[0].group", "binding group bound");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kElement, R"("binding":0)", R"("binding":256)")),
                 ValidationErrorCode::OutOfRange, "$.shaders[0].bindings[0].binding",
                 "binding index bound");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("size":16)", R"("size":0)")),
        ValidationErrorCode::InvalidValue, "$.shaders[0].pushConstants.size",
        "zero push-constant size");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElement, R"("size":16)", R"("size":130)")),
        ValidationErrorCode::OutOfRange, "$.shaders[0].pushConstants.size",
        "push-constant size bound");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kElement, R"("type":"float")", R"("type":"double")")),
                 ValidationErrorCode::InvalidValue, "$.shaders[0].pushConstants.fields[0].type",
                 "unknown push-constant type");
}

void test_oversized_and_deep(Checker& checker) {
    const std::string oversized(tessera::schema::kMaxManifestBytes + 1, ' ');
    expect_error(checker, tessera::schema::parse_manifest(oversized),
                 ValidationErrorCode::FileTooLarge, "$", "oversized manifest");

    std::string deep = R"({"manifestVersion":1,"shaders":)";
    constexpr std::size_t kNesting = 40;
    deep.append(kNesting, '[');
    deep.append(kNesting, ']');
    deep.push_back('}');
    expect_error(checker, tessera::schema::parse_manifest(deep), ValidationErrorCode::JsonTooDeep,
                 "$", "deeply nested manifest");
}

void test_strict_json(Checker& checker) {
    expect_error(checker, tessera::schema::parse_manifest(""), ValidationErrorCode::InvalidJson,
                 "$", "empty input");
    expect_error(checker, tessera::schema::parse_manifest("[]"), ValidationErrorCode::RootNotObject,
                 "$", "array root");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     R"({"manifestVersion":1,"manifestVersion":1,"shaders":[]})"),
                 ValidationErrorCode::InvalidJson, "$", "duplicate key");
    expect_error(checker,
                 tessera::schema::parse_manifest(R"({"manifestVersion":1,"shaders":[],"x":"\q"})"),
                 ValidationErrorCode::InvalidJson, "$", "invalid escape");
    expect_error(checker,
                 tessera::schema::parse_manifest(R"({"manifestVersion":1,"shaders":[]} x)"),
                 ValidationErrorCode::InvalidJson, "$", "trailing content");
    expect_error(checker,
                 tessera::schema::parse_manifest(R"({"manifestVersion":1,"shaders":[],"x":")" +
                                                 std::string(1, static_cast<char>(0xFF)) + R"("})"),
                 ValidationErrorCode::InvalidJson, "$", "invalid UTF-8");

    const auto first = tessera::schema::parse_manifest(with_stages(R"("stages":["compute"])"));
    const auto second = tessera::schema::parse_manifest(with_stages(R"("stages":["compute"])"));
    checker.check(first.has_value() == second.has_value(), "error determinism: value presence");
    checker.check(!first && !second && first.error() == second.error(),
                  "error determinism: identical errors");
    checker.check(!first && first.error().offset == 0, "validation errors carry offset 0");

    const auto bad =
        tessera::schema::parse_manifest(R"({"manifestVersion":1,"manifestVersion":1})");
    checker.check(!bad && bad.error().offset > 0, "JSON errors carry a byte offset");
}

} // namespace

int main() {
    Checker checker;
    test_version(checker);
    test_bounds(checker);
    test_field_bounds(checker);
    test_oversized_and_deep(checker);
    test_strict_json(checker);
    return checker.finish();
}
