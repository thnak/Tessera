// SPDX-License-Identifier: MIT

// Per-entry shape validation for the shader manifest: the closed key set, required fields, value
// types and scalar bounds of docs/specs/shader-manifest-v1.md sections 2-3. Section 4 semantic
// rules run afterwards in shader_manifest_rules.cpp.

#include "shader_manifest_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::schema::internal {
namespace {

std::optional<ShaderStage> stage_from_token(std::string_view token) {
    if (token == "vertex") {
        return ShaderStage::Vertex;
    }
    if (token == "fragment") {
        return ShaderStage::Fragment;
    }
    if (token == "compute") {
        return ShaderStage::Compute;
    }
    return std::nullopt;
}

std::string_view source_name_detail() {
    return "source must be a bare file name";
}

bool is_bare_file_name(std::string_view text) {
    return text.find_first_of("/\\:") == std::string_view::npos &&
           text.find("..") == std::string_view::npos;
}

std::expected<void, ValidationError> parse_stages(const JsonValue& value, std::string_view path,
                                                  ShaderEntry& entry) {
    const auto* array = value.as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an array");
    }
    if (array->empty()) {
        return fail(ValidationErrorCode::InvalidValue, std::string(path),
                    "at least one stage is required");
    }
    if (array->size() > kMaxStageCount) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "too many stages");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(path, index);
        if (!(*array)[index].is_string()) {
            return fail(ValidationErrorCode::WrongType, item_path, "expected a string");
        }
        const std::string_view token = (*array)[index].as_string();
        const auto stage = stage_from_token(token);
        if (!stage) {
            return fail(ValidationErrorCode::UnknownStage, item_path, "unknown stage");
        }
        if (*stage == ShaderStage::Compute) {
            return fail(ValidationErrorCode::ComputeStageUnsupported, item_path,
                        "compute stages are not in V1");
        }
        entry.stages.push_back(*stage);
    }
    return {};
}

std::expected<void, ValidationError> parse_entry_points(const JsonValue& value,
                                                        std::string_view path, ShaderEntry& entry) {
    const auto* members = value.as_object();
    if (members == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an object");
    }
    std::vector<std::pair<ShaderStage, std::string>> explicit_entries;
    for (const auto& member : *members) {
        const std::string item_path = member_path(path, member.first);
        const auto stage = stage_from_token(member.first);
        const bool declared =
            stage && *stage != ShaderStage::Compute &&
            std::find(entry.stages.begin(), entry.stages.end(), *stage) != entry.stages.end();
        if (!declared) {
            return fail(ValidationErrorCode::EntryStageNotDeclared, item_path,
                        "entry key is not a declared stage");
        }
        auto name = string_value(member.second, item_path, kMaxEntryPointLength);
        if (!name) {
            return std::unexpected(name.error());
        }
        explicit_entries.emplace_back(*stage, std::move(*name));
    }
    for (ShaderStage stage : entry.stages) {
        const auto found = std::find_if(explicit_entries.begin(), explicit_entries.end(),
                                        [&](const auto& item) { return item.first == stage; });
        entry.entries.push_back(StageEntry{
            stage, found == explicit_entries.end() ? std::string("main") : found->second});
    }
    return {};
}

std::expected<void, ValidationError>
parse_source_list(const JsonValue& value, std::string_view path, std::vector<std::string>& out) {
    const auto* array = value.as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an array");
    }
    if (array->empty()) {
        return fail(ValidationErrorCode::InvalidValue, std::string(path),
                    "at least one source file is required");
    }
    if (array->size() > kMaxSourceCount) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "too many source files");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(path, index);
        auto name = string_value((*array)[index], item_path, kMaxSourceNameLength);
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!is_bare_file_name(*name)) {
            return fail(ValidationErrorCode::InvalidValue, item_path, source_name_detail());
        }
        out.push_back(std::move(*name));
    }
    return {};
}

std::expected<void, ValidationError> parse_sources(const JsonValue& value, std::string_view path,
                                                   ShaderEntry& entry) {
    constexpr std::string_view kSourceKeys[] = {"vulkan", "webgpu"};
    if (auto keys = check_keys(value, kSourceKeys, path); !keys) {
        return keys;
    }
    auto vulkan = required_member(value, "vulkan", path);
    if (!vulkan) {
        return std::unexpected(vulkan.error());
    }
    if (auto result =
            parse_source_list(**vulkan, member_path(path, "vulkan"), entry.vulkan_sources);
        !result) {
        return result;
    }
    if (const JsonValue* webgpu = find_member(value, "webgpu")) {
        return parse_source_list(*webgpu, member_path(path, "webgpu"), entry.webgpu_sources);
    }
    return {};
}

std::expected<ShaderKind, ValidationError> parse_kind(const JsonValue& value,
                                                      std::string_view path) {
    if (!value.is_string()) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected a string");
    }
    const std::string_view token = value.as_string();
    if (token == "element") {
        return ShaderKind::Element;
    }
    if (token == "view-transition-effect") {
        return ShaderKind::ViewTransitionEffect;
    }
    return fail(ValidationErrorCode::InvalidValue, std::string(path), "unknown shader kind");
}

} // namespace

std::expected<ParsedShaderEntry, ValidationError> parse_shader_entry(const JsonValue& value,
                                                                     std::string_view path) {
    constexpr std::string_view kShaderKeys[] = {"name",     "kind",          "stages",
                                                "entry",    "sources",       "vertexLayout",
                                                "bindings", "pushConstants", "tests"};
    if (!value.is_object()) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an object");
    }
    if (auto keys = check_keys(value, kShaderKeys, path); !keys) {
        return std::unexpected(keys.error());
    }

    ParsedShaderEntry parsed;
    auto name = required_string(value, "name", path, kMaxShaderNameLength);
    if (!name) {
        return std::unexpected(name.error());
    }
    parsed.entry.name = std::move(*name);

    if (const JsonValue* kind = find_member(value, "kind")) {
        auto parsed_kind = parse_kind(*kind, member_path(path, "kind"));
        if (!parsed_kind) {
            return std::unexpected(parsed_kind.error());
        }
        parsed.entry.kind = *parsed_kind;
    }

    auto stages = required_member(value, "stages", path);
    if (!stages) {
        return std::unexpected(stages.error());
    }
    if (auto result = parse_stages(**stages, member_path(path, "stages"), parsed.entry); !result) {
        return std::unexpected(result.error());
    }

    if (const JsonValue* entry_points = find_member(value, "entry")) {
        if (auto result =
                parse_entry_points(*entry_points, member_path(path, "entry"), parsed.entry);
            !result) {
            return std::unexpected(result.error());
        }
    } else {
        for (ShaderStage stage : parsed.entry.stages) {
            parsed.entry.entries.push_back(StageEntry{stage, "main"});
        }
    }

    auto sources = required_member(value, "sources", path);
    if (!sources) {
        return std::unexpected(sources.error());
    }
    if (auto result = parse_sources(**sources, member_path(path, "sources"), parsed.entry);
        !result) {
        return std::unexpected(result.error());
    }

    if (const JsonValue* layout = find_member(value, "vertexLayout")) {
        parsed.has_vertex_layout = true;
        if (auto result =
                parse_vertex_layout(*layout, member_path(path, "vertexLayout"), parsed.entry);
            !result) {
            return std::unexpected(result.error());
        }
    }
    if (const JsonValue* bindings = find_member(value, "bindings")) {
        if (auto result = parse_bindings(*bindings, member_path(path, "bindings"), parsed.entry);
            !result) {
            return std::unexpected(result.error());
        }
    }
    if (const JsonValue* constants = find_member(value, "pushConstants")) {
        if (auto result =
                parse_push_constants(*constants, member_path(path, "pushConstants"), parsed.entry);
            !result) {
            return std::unexpected(result.error());
        }
    }

    auto tests = required_member(value, "tests", path);
    if (!tests) {
        return std::unexpected(tests.error());
    }
    if (auto result = parse_tests(**tests, member_path(path, "tests"), parsed.entry); !result) {
        return std::unexpected(result.error());
    }
    return parsed;
}

} // namespace tessera::schema::internal
