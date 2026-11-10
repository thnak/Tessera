// SPDX-License-Identifier: MIT

// Shader manifest key set and value-type rules (docs/specs/shader-manifest-v1.md sections 2-3):
// every object rejects keys outside its closed set, required keys are reported with a stable field
// path, and a wrong JSON type is a typed error rather than a silent default.

#include "manifest_test_util.hpp"
#include "tessera/schema/manifest.hpp"

#include <string>
#include <string_view>

namespace {

using manifest_test::Checker;
using manifest_test::erase_first;
using manifest_test::expect_error;
using manifest_test::replace_first;
using tessera::schema::ValidationErrorCode;

constexpr std::string_view kMinimalManifest =
    R"json({"manifestVersion":1,"shaders":[{"name":"minimal","stages":["fragment"],"sources":{"vulkan":["minimal.frag"]},"tests":[{"name":"smoke"}]}]})json";

constexpr std::string_view kElementManifest =
    R"json({"manifestVersion":1,"shaders":[{"name":"batched2d","stages":["vertex","fragment"],"sources":{"vulkan":["batched2d.vert"]},"vertexLayout":[{"location":0,"format":"float2","semantic":"position"}],"bindings":[{"group":0,"binding":0,"type":"uniform_buffer","name":"push"}],"pushConstants":{"size":16,"fields":[{"name":"viewport_width","type":"float","offset":0}]},"tests":[{"name":"solid_quad","golden":"tests/golden/gpu/shaders/batched2d_solid.png"}]}]})json";

void test_missing_keys(Checker& checker) {
    const std::string minimal(kMinimalManifest);
    expect_error(checker,
                 tessera::schema::parse_manifest(erase_first(minimal, R"("manifestVersion":1,)")),
                 ValidationErrorCode::MissingKey, "$.manifestVersion", "missing manifestVersion");
    expect_error(checker, tessera::schema::parse_manifest(R"({"manifestVersion":1})"),
                 ValidationErrorCode::MissingKey, "$.shaders", "missing shaders key");
    expect_error(checker,
                 tessera::schema::parse_manifest(erase_first(minimal, R"("name":"minimal",)")),
                 ValidationErrorCode::MissingKey, "$.shaders[0].name", "missing shader name");
    expect_error(checker,
                 tessera::schema::parse_manifest(erase_first(minimal, R"("stages":["fragment"],)")),
                 ValidationErrorCode::MissingKey, "$.shaders[0].stages", "missing stages");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     erase_first(minimal, R"(,"sources":{"vulkan":["minimal.frag"]})")),
                 ValidationErrorCode::MissingKey, "$.shaders[0].sources", "missing sources");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(minimal, R"("vulkan":["minimal.frag"])", R"("vulkan":[])")),
                 ValidationErrorCode::InvalidValue, "$.shaders[0].sources.vulkan",
                 "empty Vulkan source list");
    expect_error(
        checker,
        tessera::schema::parse_manifest(erase_first(minimal, R"(,"tests":[{"name":"smoke"}])")),
        ValidationErrorCode::MissingKey, "$.shaders[0].tests", "missing tests");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     minimal, R"("vulkan":["minimal.frag"])", R"("webgpu":["x.wgsl"])")),
                 ValidationErrorCode::MissingKey, "$.shaders[0].sources.vulkan",
                 "missing Vulkan sources");
}

void test_unknown_keys(Checker& checker) {
    const std::string minimal(kMinimalManifest);
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     minimal, R"({"manifestVersion":1,)", R"({"manifestVersion":1,"extra":0,)")),
                 ValidationErrorCode::UnknownKey, "$.extra", "unknown root key");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     minimal, R"("stages":["fragment"],)", R"("stages":["fragment"],"extra":0,)")),
                 ValidationErrorCode::UnknownKey, "$.shaders[0].extra", "unknown shader key");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(minimal, R"("vulkan":["minimal.frag"])",
                                                      R"("vulkan":["minimal.frag"],"extra":0)")),
        ValidationErrorCode::UnknownKey, "$.shaders[0].sources.extra", "unknown sources key");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(minimal, R"({"name":"smoke"})",
                                                               R"({"name":"smoke","extra":0})")),
                 ValidationErrorCode::UnknownKey, "$.shaders[0].tests[0].extra",
                 "unknown test key");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kElementManifest, R"({"location":0,"format":"float2","semantic":"position"})",
                     R"({"location":0,"format":"float2","semantic":"position","extra":0})")),
                 ValidationErrorCode::UnknownKey, "$.shaders[0].vertexLayout[0].extra",
                 "unknown vertex attribute key");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(
            kElementManifest, R"({"group":0,"binding":0,"type":"uniform_buffer","name":"push"})",
            R"({"group":0,"binding":0,"type":"uniform_buffer","name":"push","extra":0})")),
        ValidationErrorCode::UnknownKey, "$.shaders[0].bindings[0].extra", "unknown binding key");
    expect_error(checker,
                 tessera::schema::parse_manifest(replace_first(
                     kElementManifest, R"({"name":"viewport_width","type":"float","offset":0})",
                     R"({"name":"viewport_width","type":"float","offset":0,"extra":0})")),
                 ValidationErrorCode::UnknownKey, "$.shaders[0].pushConstants.fields[0].extra",
                 "unknown push-constant field key");
}

void test_wrong_types(Checker& checker) {
    const std::string minimal(kMinimalManifest);
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(minimal, R"("manifestVersion":1)", R"("manifestVersion":"1")")),
                 ValidationErrorCode::WrongType, "$.manifestVersion", "manifestVersion as string");
    expect_error(checker, tessera::schema::parse_manifest(R"({"manifestVersion":1,"shaders":{}})"),
                 ValidationErrorCode::WrongType, "$.shaders", "shaders as object");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(minimal, R"("stages":["fragment"])", R"("stages":"fragment")")),
                 ValidationErrorCode::WrongType, "$.shaders[0].stages", "stages as string");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(minimal, R"("name":"minimal")", R"("name":1)")),
                 ValidationErrorCode::WrongType, "$.shaders[0].name", "name as number");
    expect_error(checker,
                 tessera::schema::parse_manifest(
                     replace_first(kElementManifest, R"("location":0)", R"("location":"0")")),
                 ValidationErrorCode::WrongType, "$.shaders[0].vertexLayout[0].location",
                 "location as string");
    expect_error(
        checker,
        tessera::schema::parse_manifest(replace_first(kElementManifest, R"("name":"batched2d",)",
                                                      R"("name":"batched2d","kind":7,)")),
        ValidationErrorCode::WrongType, "$.shaders[0].kind", "kind as number");
}

} // namespace

int main() {
    Checker checker;
    test_missing_keys(checker);
    test_unknown_keys(checker);
    test_wrong_types(checker);
    return checker.finish();
}
