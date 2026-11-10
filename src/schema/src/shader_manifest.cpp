// SPDX-License-Identifier: MIT

// Shader manifest entry point: document shape, versioning and the entry table
// (docs/specs/shader-manifest-v1.md sections 1-3). Per-shader field validation lives in
// shader_manifest_entry.cpp; section 4 compatibility rules live in shader_manifest_rules.cpp.

#include "shader_manifest_internal.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace tessera::schema {
namespace {

constexpr std::string_view kRootKeys[] = {"manifestVersion", "shaders"};

} // namespace

namespace internal {

std::unexpected<ValidationError> fail(ValidationErrorCode code, std::string path,
                                      std::string_view detail) {
    return std::unexpected(ValidationError{code, std::move(path), detail, 0});
}

std::string member_path(std::string_view path, std::string_view key) {
    std::string result(path);
    result.push_back('.');
    result.append(key);
    return result;
}

std::string element_path(std::string_view path, std::size_t index) {
    std::string result(path);
    result.push_back('[');
    result.append(std::to_string(index));
    result.push_back(']');
    return result;
}

const JsonValue* find_member(const JsonValue& object, std::string_view key) {
    const auto* members = object.as_object();
    if (members == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(
        *members, [&](const JsonValue::Member& member) { return member.first == key; });
    return found == members->end() ? nullptr : &found->second;
}

std::expected<void, ValidationError> check_keys(const JsonValue& object,
                                                std::span<const std::string_view> allowed,
                                                std::string_view path) {
    const auto* members = object.as_object();
    if (members == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an object");
    }
    for (const auto& member : *members) {
        if (std::ranges::find(allowed, member.first) == allowed.end()) {
            return fail(ValidationErrorCode::UnknownKey, member_path(path, member.first),
                        "unknown key for this object");
        }
    }
    return {};
}

namespace {

std::string_view json_error_detail(JsonErrorCode code) {
    switch (code) {
    case JsonErrorCode::UnexpectedEnd:
        return "unexpected end of input";
    case JsonErrorCode::UnexpectedCharacter:
        return "unexpected character";
    case JsonErrorCode::TrailingContent:
        return "trailing content after the document";
    case JsonErrorCode::InvalidLiteral:
        return "invalid literal";
    case JsonErrorCode::InvalidNumber:
        return "invalid number literal";
    case JsonErrorCode::NumberOutOfRange:
        return "number is out of range";
    case JsonErrorCode::InvalidEscape:
        return "invalid escape sequence";
    case JsonErrorCode::InvalidUtf16Escape:
        return "invalid UTF-16 escape";
    case JsonErrorCode::ControlCharacter:
        return "raw control character in a string";
    case JsonErrorCode::DuplicateKey:
        return "duplicate object key";
    case JsonErrorCode::NestingTooDeep:
        return "nesting is too deep";
    case JsonErrorCode::InvalidUtf8:
        return "string is not valid UTF-8";
    }
    return "invalid JSON";
}

ValidationError json_error(const JsonParseError& error) {
    const ValidationErrorCode code = error.code == JsonErrorCode::NestingTooDeep
                                         ? ValidationErrorCode::JsonTooDeep
                                         : ValidationErrorCode::InvalidJson;
    return ValidationError{code, "$", json_error_detail(error.code), error.offset};
}

} // namespace

std::uint32_t push_constant_type_size(std::string_view type) noexcept {
    if (type == "float" || type == "int" || type == "uint") {
        return 4;
    }
    if (type == "float2") {
        return 8;
    }
    if (type == "float3") {
        return 12;
    }
    if (type == "float4") {
        return 16;
    }
    return 0;
}

} // namespace internal

std::expected<Manifest, ValidationError> parse_manifest(std::string_view json_text) {
    using namespace internal;

    if (json_text.size() > kMaxManifestBytes) {
        return fail(ValidationErrorCode::FileTooLarge, "$", "manifest exceeds the byte limit");
    }

    auto document = parse_json(json_text, kMaxManifestDepth);
    if (!document) {
        return std::unexpected(json_error(document.error()));
    }
    if (!document->is_object()) {
        return fail(ValidationErrorCode::RootNotObject, "$", "expected an object");
    }
    if (auto keys = check_keys(*document, kRootKeys, "$"); !keys) {
        return std::unexpected(keys.error());
    }

    const JsonValue* version_value = find_member(*document, "manifestVersion");
    if (version_value == nullptr) {
        return fail(ValidationErrorCode::MissingKey, "$.manifestVersion",
                    "required key is missing");
    }
    const auto version = version_value->as_integer();
    if (!version) {
        return fail(ValidationErrorCode::WrongType, "$.manifestVersion", "expected an integer");
    }
    if (std::cmp_not_equal(*version, kManifestVersion)) {
        return fail(ValidationErrorCode::UnsupportedVersion, "$.manifestVersion",
                    "only manifest version 1 is supported");
    }

    const JsonValue* shaders_value = find_member(*document, "shaders");
    if (shaders_value == nullptr) {
        return fail(ValidationErrorCode::MissingKey, "$.shaders", "required key is missing");
    }
    const auto* shaders = shaders_value->as_array();
    if (shaders == nullptr) {
        return fail(ValidationErrorCode::WrongType, "$.shaders", "expected an array");
    }
    if (shaders->size() > kMaxShaderCount) {
        return fail(ValidationErrorCode::OutOfRange, "$.shaders", "too many shader entries");
    }

    Manifest manifest;
    manifest.version = static_cast<std::uint32_t>(*version);
    manifest.shaders.reserve(shaders->size());
    for (std::size_t index = 0; index < shaders->size(); ++index) {
        const std::string path = element_path("$.shaders", index);
        auto entry = parse_shader_entry((*shaders)[index], path);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (auto rules = apply_shader_rules(*entry, path); !rules) {
            return std::unexpected(rules.error());
        }
        for (const ShaderEntry& existing : manifest.shaders) {
            if (existing.name == entry->entry.name) {
                return fail(ValidationErrorCode::DuplicateShaderName, member_path(path, "name"),
                            "shader names must be unique");
            }
        }
        manifest.shaders.push_back(std::move(entry->entry));
    }
    return manifest;
}

} // namespace tessera::schema
